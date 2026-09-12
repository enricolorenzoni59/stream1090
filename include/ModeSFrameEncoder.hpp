/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "ModeSFrame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

struct EncodedModeSFrame {
    std::array<uint8_t, 64> bytes{};
    size_t size = 0;
};

namespace ModeSFrameEncoderDetail {
inline constexpr char Hex[] = "0123456789ABCDEF";

inline void appendHex(EncodedModeSFrame& out, uint64_t value, unsigned digits) noexcept {
    for (unsigned shift = digits * 4; shift != 0; shift -= 4)
        out.bytes[out.size++] = uint8_t(Hex[(value >> (shift - 4)) & 0xF]);
}

inline void appendEscaped(EncodedModeSFrame& out, uint8_t byte) noexcept {
    out.bytes[out.size++] = byte;
    if (byte == 0x1A)
        out.bytes[out.size++] = byte;
}

inline void appendBigEndianEscaped(EncodedModeSFrame& out, uint64_t value, unsigned bytes) noexcept {
    for (unsigned shift = bytes * 8; shift != 0; shift -= 8)
        appendEscaped(out, uint8_t(value >> (shift - 8)));
}
} // namespace ModeSFrameEncoderDetail

inline EncodedModeSFrame encodeAvr(const ModeSFrame& frame) noexcept {
    using namespace ModeSFrameEncoderDetail;
    EncodedModeSFrame out;
    out.bytes[out.size++] = frame.signalAvailable ? '<' : '@';
    appendHex(out, frame.mlatTimestamp & 0xFFFFFFFFFFFFULL, 12);
    if (frame.signalAvailable)
        appendHex(out, frame.signalLevel, 2);
    if (frame.length == ModeSFrameLength::Short) {
        appendHex(out, frame.low & 0x00FFFFFFFFFFFFFFULL, 14);
    } else {
        appendHex(out, frame.high & 0xFFFFFFFFFFFFULL, 12);
        appendHex(out, frame.low, 16);
    }
    out.bytes[out.size++] = ';';
    out.bytes[out.size++] = '\n';
    return out;
}

inline EncodedModeSFrame encodeBeast(const ModeSFrame& frame) noexcept {
    using namespace ModeSFrameEncoderDetail;
    EncodedModeSFrame out;
    out.bytes[out.size++] = 0x1A;
    out.bytes[out.size++] = frame.length == ModeSFrameLength::Short ? '2' : '3';
    appendBigEndianEscaped(out, frame.mlatTimestamp & 0xFFFFFFFFFFFFULL, 6);
    appendEscaped(out, frame.signalAvailable ? frame.signalLevel : 0);
    if (frame.length == ModeSFrameLength::Short) {
        appendBigEndianEscaped(out, frame.low & 0x00FFFFFFFFFFFFFFULL, 7);
    } else {
        appendBigEndianEscaped(out, frame.high & 0xFFFFFFFFFFFFULL, 6);
        appendBigEndianEscaped(out, frame.low, 8);
    }
    return out;
}
