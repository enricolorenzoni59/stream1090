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
    // Interleave two unknown aircraft. Confirming the second one first must not
    // cause either held first sighting to be emitted behind newer RF time.
    const auto first = makeDF17(0xabcdef, 1);
    const auto second = makeDF17(0xabcdef, 2, 7);
    const auto otherFirst = makeDF17(0x123456, 1);
    const auto otherSecond = makeDF17(0x123456, 2, 7);
    auto repairCandidate = makeDF17(0xabcdef, 3);
    repairCandidate.flip(30);

    CapturingHandler handler;
    DemodCore<1, CapturingHandler> demod(handler);
    feedFrame(demod, first);
    feedSilence(demod, 128);
    feedFrame(demod, otherFirst);
    feedSilence(demod, 128);
    feedFrame(demod, repairCandidate);
    if (handler.longCount != 0)
        return 1;

    // Only each current confirming observation is emitted. The historical
    // first sightings remain internal evidence and output stays monotonic.
    feedSilence(demod, 128);
    feedFrame(demod, otherSecond);
    feedSilence(demod, 128);
    feedFrame(demod, second);
    if (!(handler.longCount == 2 && handler.frames[0] == otherSecond && handler.frames[1] == second &&
          handler.sampleTimes[0] <= handler.sampleTimes[1]))
        return 2;

    // If no confirmation arrives inside the window the held frame is dropped.
    CapturingHandler expiredHandler;
    DemodCore<1, CapturingHandler> expiredDemod(expiredHandler);
    feedFrame(expiredDemod, first);
    feedSilence(expiredDemod, 2'000'001);
    feedFrame(expiredDemod, second);
    if (!(expiredHandler.longCount == 1 && expiredHandler.frames[0] == second))
        return 3;

    return 0;
}
