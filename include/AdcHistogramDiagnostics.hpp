/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <array>
#include <cstdint>
#include <cmath>

// A compact view of the raw, interleaved 8-bit I/Q samples. Each bin covers
// 16 ADC codes; the full 256-code histogram remains available to the caller.
struct AdcHistogramDiagnostics {
    std::array<uint64_t, 16> bins{};
    uint64_t samples = 0;
    double centerPercent = 0;
    double railPercent = 0;
    double rmsLsb = 0;
    int absP99Lsb = 0;
    int absP999Lsb = 0;
};

inline uint64_t accumulateSampledAdcHistogram(const uint8_t* bytes, uint32_t length,
                                              uint32_t (&hist)[256]) {
    uint64_t counted = 0;
    // Two complete I/Q pairs in each group of eight pairs. Keeping both
    // channels matters when a blocker or the tuner makes I and Q asymmetric.
    for (uint32_t i = 0; i + 9 < length; i += 16) {
        ++hist[bytes[i]];
        ++hist[bytes[i + 1]];
        ++hist[bytes[i + 8]];
        ++hist[bytes[i + 9]];
        counted += 4;
    }
    return counted;
}

inline AdcHistogramDiagnostics summarizeAdcHistogram(const uint64_t (&hist)[256]) {
    AdcHistogramDiagnostics result;
    uint64_t center = 0, rails = 0, sumSquares = 0;
    std::array<uint64_t, 129> absHist{};

    for (int code = 0; code < 256; ++code) {
        const uint64_t count = hist[code];
        const int distance = std::abs(code - 128);
        result.bins[code / 16] += count;
        result.samples += count;
        absHist[distance] += count;
        sumSquares += count * static_cast<uint64_t>(distance * distance);
        if (distance <= 1)
            center += count;
        if (code <= 2 || code >= 254)
            rails += count;
    }
    if (result.samples == 0)
        return result;

    result.centerPercent = 100.0 * center / result.samples;
    result.railPercent = 100.0 * rails / result.samples;
    result.rmsLsb = std::sqrt(static_cast<double>(sumSquares) / result.samples);

    uint64_t seen = 0;
    bool foundP99 = false, foundP999 = false;
    for (int distance = 0; distance <= 128; ++distance) {
        seen += absHist[distance];
        if (!foundP99 && seen * 100 >= result.samples * 99) {
            result.absP99Lsb = distance;
            foundP99 = true;
        }
        if (!foundP999 && seen * 1000 >= result.samples * 999) {
            result.absP999Lsb = distance;
            foundP999 = true;
        }
    }
    return result;
}
