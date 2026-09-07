/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "DemodCore.hpp"

#include <array>
#include <cstdint>

namespace {

struct CapturingHandler {
    void handleShort(uint64_t, uint64_t) {}

    void handleLong(uint64_t sampleTime, const Bits128& frame) {
        if (longCount < frames.size()) {
            sampleTimes[longCount] = sampleTime;
            frames[longCount] = frame;
        }
        ++longCount;
    }

    uint32_t longCount{0};
    std::array<uint64_t, 4> sampleTimes{};
    std::array<Bits128, 4> frames{Bits128(), Bits128(), Bits128(), Bits128()};
};

Bits128 makeDF17(uint32_t icao, uint8_t typeCode, uint8_t capability = 5) {
    const uint64_t high =
        (uint64_t(17) << 43) | (uint64_t(capability) << 40) | (uint64_t(icao) << 16) | (uint64_t(typeCode) << 11);
    Bits128 frame(high, 0);
    frame.low() = CRC::compute<112>(frame);
    return frame;
}

void feedSilence(DemodCore<1, CapturingHandler>& demod, uint32_t bits) {
    for (uint32_t bit = 0; bit < bits; ++bit) {
        uint32_t value[] = {0};
        demod.shiftInNewBits(value);
    }
}

void feedFrame(DemodCore<1, CapturingHandler>& demod, const Bits128& frame) {
    for (int bit = 111; bit >= 0; --bit) {
        uint32_t value[] = {uint32_t(frame.get(bit))};
        demod.shiftInNewBits(value);
    }
    feedSilence(demod, 16);
}

} // namespace

int main() {
    // A first sighting of an unknown address is not emitted: trust reaches the
    // trusted set only on a second, separate sighting.
    {
        CapturingHandler handler;
        DemodCore<1, CapturingHandler> demod(handler);
        const auto first = makeDF17(0x1638f5, 1);
        feedFrame(demod, first);
        feedSilence(demod, 128);
        if (handler.longCount != 0)
            return 1;

        // The validating second sighting of the same address hits the known
        // branch and is the only observation that reaches the output.
        const auto second = makeDF17(0x1638f5, 2);
        feedFrame(demod, second);
        if (!(handler.longCount == 1 && handler.frames[0] == second))
            return 2;
    }

    // Interleaved unknowns: each is held on its first sighting and released
    // only by its own confirming second sighting, so the output stays strictly
    // in RF-time order. A CRC-damaged frame is never repaired into an untrusted
    // address, and so cannot establish one.
    {
        CapturingHandler handler;
        DemodCore<1, CapturingHandler> demod(handler);
        const auto a1 = makeDF17(0x5170aa, 1);
        const auto a2 = makeDF17(0x5170aa, 2);
        const auto o1 = makeDF17(0x7e0a02, 1, 7);
        const auto o2 = makeDF17(0x7e0a02, 2, 7);
        auto damaged = makeDF17(0xa1b2c3, 3, 7);
        damaged.flip(30);
        feedFrame(demod, a1);
        feedFrame(demod, o1);
        feedFrame(demod, damaged);
        if (handler.longCount != 0)
            return 3;

        feedSilence(demod, 128);
        feedFrame(demod, a2);
        if (!(handler.longCount == 1 && handler.frames[0] == a2))
            return 4;

        feedSilence(demod, 128);
        feedFrame(demod, o2);
        if (!(handler.longCount == 2 && handler.frames[1] == o2 && handler.sampleTimes[0] <= handler.sampleTimes[1]))
            return 5;
    }

    return 0;
}