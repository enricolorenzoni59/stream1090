/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

namespace RtlSdrSerial {

inline bool parsePositiveDecimal(const std::string& serial,
                                 uint64_t& value) noexcept {
    if (serial.empty())
        return false;

    value = 0;
    const char* first = serial.data();
    const char* last = first + serial.size();
    const auto result = std::from_chars(first, last, value, 10);
    return result.ec == std::errc{} && result.ptr == last && value != 0;
}

inline int resolveIndex(const std::string& requested,
                        const std::vector<std::string>& available) noexcept {
    // RTL-SDR serials are opaque strings. Do not reinterpret a valid exact
    // match merely because it happens to look decimal or hexadecimal.
    for (std::size_t i = 0; i < available.size(); ++i) {
        if (available[i] == requested)
            return static_cast<int>(i);
    }

    // Preserve compatibility with old configurations that used e.g.
    // "serial = 2" for a dongle whose EEPROM contains "00000002".
    uint64_t requestedValue = 0;
    if (!parsePositiveDecimal(requested, requestedValue))
        return -1;

    for (std::size_t i = 0; i < available.size(); ++i) {
        uint64_t availableValue = 0;
        if (parsePositiveDecimal(available[i], availableValue)
                && availableValue == requestedValue)
            return static_cast<int>(i);
    }

    return -1;
}

} // namespace RtlSdrSerial
