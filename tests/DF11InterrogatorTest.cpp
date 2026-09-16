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

    uint32_t shortCount{0};
    uint64_t lastShort{0};
};

uint64_t makeDF11(uint32_t icao, uint8_t interrogatorCode) {
    constexpr uint8_t capability = 5;
    uint64_t frame = (uint64_t(11) << 51) | (uint64_t(capability) << 48) | (uint64_t(icao) << 24);
    const auto parity = CRC::compute<56>(Bits128(frame)) ^ interrogatorCode;
    return frame | parity;
}

void feedFrame(DemodCore<1, CapturingHandler>& demod, uint64_t frame) {
    for (int bit = 55; bit >= 0; --bit) {
        uint32_t value[] = {uint32_t((frame >> bit) & 1)};
        demod.shiftInNewBits(value);
    }

    for (int bit = 0; bit < 72; ++bit) {
        uint32_t value[] = {0};
        demod.shiftInNewBits(value);
    }
}

} // namespace

int main() {
    constexpr uint32_t icao = 0xabcdef;
    const auto first = makeDF11(icao, 1);
    const auto second = makeDF11(icao, 22);
    const auto third = makeDF11(icao, 79);

    if (CRC::compute<56>(Bits128(first)) != 1 || CRC::compute<56>(Bits128(second)) != 22 ||
        CRC::compute<56>(Bits128(third)) != 79)
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

    // Establish clean ES trust, then present a DF11 with an intact identity
    // but three damaged parity bits. A cached identity must not license an
    // arbitrary rewrite of the entire parity field.
    CapturingHandler guardedHandler;
    DemodCore<1, CapturingHandler> guarded(guardedHandler);
    for (uint8_t typeCode : {1, 2}) {
        Bits128 es((uint64_t(17) << 43) | (uint64_t(5) << 40) |
                       (uint64_t(icao) << 16) | (uint64_t(typeCode) << 11), 0);
        es.low() = CRC::compute<112>(es);
        for (int bit = 111; bit >= 0; --bit) {
            uint32_t value[] = {uint32_t(es.get(bit))};
            guarded.shiftInNewBits(value);
        }
        for (int bit = 0; bit < 128; ++bit) {
            uint32_t value[] = {0};
            guarded.shiftInNewBits(value);
        }
    }
    const auto clean = makeDF11(icao, 0);
    const auto corrupt = clean ^ 0x808080;
    const auto syndrome = CRC::compute<56>(Bits128(corrupt));
    if (syndrome < 80 || CRC::df11ErrorTable.lookup(syndrome).valid())
        return 2;
    feedFrame(guarded, corrupt);
    if (guardedHandler.shortCount != 0)
        return 3;
    // A bounded single-bit parity repair remains available.
    feedFrame(guarded, clean ^ (uint64_t(1) << 15));
    if (guardedHandler.shortCount != 1 || guardedHandler.lastShort != clean)
        return 4;
    feedFrame(guarded, makeDF11(icao, 22));
    if (guardedHandler.shortCount != 2 || guardedHandler.lastShort != clean)
        return 5;
    return 0;
}
