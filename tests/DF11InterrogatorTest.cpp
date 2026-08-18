/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "DemodCore.hpp"

#include <cstdint>

namespace {

struct CapturingHandler {
    void handleShort(uint64_t, uint64_t frame) {
        shortCount++;
        lastShort = frame;
    }

    void handleLong(uint64_t, const Bits128&) {}

    uint32_t shortCount { 0 };
    uint64_t lastShort { 0 };
};

uint64_t makeDF11(uint32_t icao, uint8_t interrogatorCode) {
    constexpr uint8_t capability = 5;
    uint64_t frame = (uint64_t(11) << 51)
        | (uint64_t(capability) << 48)
        | (uint64_t(icao) << 24);
    const auto parity = CRC::compute<56>(Bits128(frame)) ^ interrogatorCode;
    return frame | parity;
}

Bits128 makeDF17(uint32_t icao) {
    constexpr uint8_t capability = 5;
    Bits128 frame((uint64_t(17) << 43)
        | (uint64_t(capability) << 40)
        | (uint64_t(icao) << 16), 0);
    frame.low() = CRC::compute<112>(frame);
    return frame;
}

void feedFrame(DemodCore<1, CapturingHandler>& demod, uint64_t frame) {
    for (int bit = 55; bit >= 0; --bit) {
        uint32_t value[] = { uint32_t((frame >> bit) & 1) };
        demod.shiftInNewBits(value);
    }

    for (int bit = 0; bit < 72; ++bit) {
        uint32_t value[] = { 0 };
        demod.shiftInNewBits(value);
    }
}

void feedLongFrame(DemodCore<1, CapturingHandler>& demod,
                   const Bits128& frame) {
    for (int bit = 111; bit >= 0; --bit) {
        uint32_t value[] = { uint32_t(frame.get(bit)) };
        demod.shiftInNewBits(value);
    }
    for (int bit = 0; bit < 128; ++bit) {
        uint32_t value[] = { 0 };
        demod.shiftInNewBits(value);
    }
}

float snr(const void*, uint8_t) {
    return 0.5f;
}

float preamble(const void* context, size_t) {
    return *static_cast<const bool*>(context) ? 2.0f : 0.0f;
}

} // namespace

int main() {
    constexpr uint32_t icao = 0xabcdef;
    const auto first = makeDF11(icao, 1);
    const auto second = makeDF11(icao, 22);
    const auto third = makeDF11(icao, 79);

    if (CRC::compute<56>(Bits128(first)) != 1
            || CRC::compute<56>(Bits128(second)) != 22
            || CRC::compute<56>(Bits128(third)) != 79)
        return 1;

    CapturingHandler handler;
    DemodCore<1, CapturingHandler> demod(handler);

    feedFrame(demod, first);
    if (handler.shortCount != 0)
        return 1;

    feedFrame(demod, second);
    if (handler.shortCount != 0)
        return 1;

    feedFrame(demod, third);
    if (handler.shortCount != 1)
        return 1;

    // The interrogator code is xored out of the parity before the frame is
    // emitted, so a downstream decoder recomputing the CRC sees zero. Assert
    // the property rather than the resulting word, so the test keeps stating
    // the contract if the frame layout ever moves.
    if (CRC::compute<56>(Bits128(handler.lastShort)) != 0)
        return 1;

    // Everything outside the parity field must be untouched.
    constexpr uint64_t parityMask = (uint64_t(1) << 24) - 1;
    if ((handler.lastShort & ~parityMask) != (third & ~parityMask))
        return 1;

    // Make the address trusted through a clean extended squitter, then drive
    // the address-parity fallback with a syndrome that is not a one-bit fix.
    feedLongFrame(demod, makeDF17(icao));
    bool hasPreamble = false;
    demod.setSnrSource(nullptr, snr);
    demod.setPreambleSource(&hasPreamble, preamble);

    const auto noiseFloorFallback = makeDF11(icao, 0) ^ 0x123456;
    if (CRC::df11ErrorTable.lookup(
            CRC::compute<56>(Bits128(noiseFloorFallback))).valid())
        return 1;
    feedFrame(demod, noiseFloorFallback);
    if (handler.shortCount != 1)
        return 1;

    hasPreamble = true;
    const auto confirmedFallback = makeDF11(icao, 0) ^ 0x654321;
    if (CRC::df11ErrorTable.lookup(
            CRC::compute<56>(Bits128(confirmedFallback))).valid())
        return 1;
    feedFrame(demod, confirmedFallback);
    return handler.shortCount == 2 ? 0 : 1;
}
