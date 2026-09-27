/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "Sampler.hpp"

// Generated once at setup, never on the sample path. Unity DC gain.
// These are reproducible starting points, not claimed optimal ADS-B filters.
inline std::vector<float> basebandTaps(SampleRate rate) {
    constexpr double pi = 3.14159265358979323846;
    const size_t count = rate == Rate_4_0_Mhz ? 9 : 17;
    const double cutoff = std::min(1550000.0, double(rate) * 0.45) / double(rate);
    std::vector<float> taps(count);
    double sum = 0;
    for (size_t n = 0; n < count; ++n) {
        const double x = double(n) - double(count - 1) / 2;
        const double sinc = x == 0 ? 2*cutoff : std::sin(2*pi*cutoff*x)/(pi*x);
        taps[n] = float(sinc * (0.54 - 0.46*std::cos(2*pi*n/(count-1))));
        sum += taps[n];
    }
    for (auto& t : taps) t = float(t / sum);
    return taps;
}
