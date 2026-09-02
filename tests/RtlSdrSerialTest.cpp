/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "devices/RtlSdrSerial.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const char* what, int got, int want) {
    if (got != want) {
        std::printf("FAIL %s: expected index %d, got %d\n", what, want, got);
        failures++;
    }
}

int resolve(const char* requested, std::initializer_list<const char*> serials) {
    std::vector<std::string> available;
    for (const char* serial : serials)
        available.emplace_back(serial);
    return RtlSdrSerial::resolveIndex(requested, available);
}

} // namespace

int main() {
    // A serial is an opaque string. Exact identity must win even when another
    // device has a numerically equivalent spelling and appears first.
    check("zero-padded exact match",
          resolve("00000002", {"2", "00000002"}), 1);
    check("short exact match",
          resolve("2", {"00000002", "2"}), 1);
    check("hex-looking exact match",
          resolve("0x1A", {"26", "0x1A"}), 1);
    check("all-zero exact match",
          resolve("00000000", {"11111111", "00000000"}), 1);
    check("alphanumeric exact match",
          resolve("ADSBX-ORANGE-1090", {"00000001", "ADSBX-ORANGE-1090"}), 1);

    // Preserve the pre-string-serial convenience: a short decimal value may
    // select a conventional zero-padded RTL-SDR serial, but only after exact
    // lookup failed.
    check("legacy decimal fallback",
          resolve("2", {"00000001", "00000002"}), 1);
    check("legacy decimal eight fallback",
          resolve("8", {"00000008", "00000009"}), 0);

    // Zero is the old sentinel for "first device", not a safe alias for an
    // explicitly requested serial. Partial or non-decimal strings must not be
    // reinterpreted either.
    check("zero has no numeric fallback",
          resolve("0", {"00000000"}), -1);
    check("partial decimal has no fallback",
          resolve("8suffix", {"00000008"}), -1);
    check("missing serial",
          resolve("missing", {"00000001", "00000002"}), -1);

    return failures == 0 ? 0 : 1;
}
