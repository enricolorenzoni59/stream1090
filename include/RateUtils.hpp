/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include "Logger.hpp"
#include "Presets.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

// The (input, output) rate pairs that actually have a preset, read from the
// compile-time preset tables.
struct RatePair {
    SampleRate in;
    SampleRate out;
};

inline std::vector<RatePair> collect_rate_pairs() {
    std::vector<RatePair> pairs;
    std::apply([&](auto... p) { ((pairs.push_back({decltype(p)::inputRate, decltype(p)::outputRate})), ...); },
               presets);

    std::sort(pairs.begin(), pairs.end(), [](auto& a, auto& b) {
        if (a.in != b.in)
            return (int)a.in < (int)b.in;
        return (int)a.out < (int)b.out;
    });

    pairs.erase(
        std::unique(pairs.begin(), pairs.end(), [](auto& a, auto& b) { return a.in == b.in && a.out == b.out; }),
        pairs.end());

    return pairs;
}

// The highest upsample offered for an input rate.
inline std::optional<SampleRate> find_default_output_rate(SampleRate input) {
    auto pairs = collect_rate_pairs();
    std::optional<SampleRate> best;
    for (auto& p : pairs) {
        if (p.in == input && (!best || (int)p.out > (int)*best))
            best = p.out;
    }
    return best;
}

inline bool is_valid_rate_pair(SampleRate in, SampleRate out) {
    auto pairs = collect_rate_pairs();
    for (auto& p : pairs) {
        if (p.in == in && p.out == out)
            return true;
    }
    return false;
}

inline bool has_input_rate(SampleRate in) {
    auto pairs = collect_rate_pairs();
    for (auto& p : pairs) {
        if (p.in == in)
            return true;
    }
    return false;
}

// The rate alone already decides the backend: RTL-SDR has no preset above
// 3.2 Msps and Airspy none below 6.
inline bool is_airspy_rate(SampleRate in) {
    return (int)in >= (int)Rate_6_0_Mhz;
}

// Non-exiting conversion from Hz to the rate enum, for integers the device
// reports (airspy_get_samplerates returns Hz).
inline bool match_sample_rate(int hz, SampleRate& out) {
    switch (hz) {
    case Rate_1_0_Mhz:
    case Rate_2_0_Mhz:
    case Rate_2_4_Mhz:
    case Rate_2_56_Mhz:
    case Rate_3_0_Mhz:
    case Rate_3_2_Mhz:
    case Rate_4_0_Mhz:
    case Rate_6_0_Mhz:
    case Rate_8_0_Mhz:
    case Rate_10_0_Mhz:
    case Rate_12_0_Mhz:
    case Rate_16_0_Mhz:
    case Rate_20_0_Mhz:
    case Rate_24_0_Mhz:
    case Rate_30_0_Mhz:
    case Rate_36_0_Mhz:
    case Rate_40_0_Mhz:
    case Rate_48_0_Mhz:
        out = static_cast<SampleRate>(hz);
        return true;
    default:
        return false;
    }
}

inline std::string rate_mhz(SampleRate rate) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(rate) / 1'000'000.0);
    return buffer;
}

inline void print_rate_pairs() {
    auto pairs = collect_rate_pairs();

    std::cout << "Supported sample rate combinations:\n";
    for (auto& p : pairs) {
#if defined(STREAM1090_CUSTOM_INPUT) && STREAM1090_CUSTOM_INPUT
        std::string fmt = "float32 IQ";
#else
        std::string fmt = (p.in < 6'000'000) ? "uint8 IQ" : "uint16 IQ";
#endif
        std::cout << "  " << (float(p.in) / 1'000'000.0f) << "  →  " << (float(p.out) / 1'000'000.0f) << " (" << fmt
                  << ")\n";
    }
    std::cout << "\n";
}

// Parses a MHz value ("2.56", "2.56M") and exits on anything without a preset-
// bearing enum value; the caller uses it only for values it has not validated.
inline SampleRate parse_sample_rate(const std::string& raw) {
    std::string s = raw;
    if (!s.empty() && (s.back() == 'M' || s.back() == 'm'))
        s.pop_back();

    float mhz = 0.0f;
    try {
        mhz = std::stof(s);
    } catch (...) {
        Log::error("Stream1090") << "Invalid sample rate: " << raw;
        std::exit(1);
    }

    const int hz = static_cast<int>(mhz * 1'000'000.0f + 0.5f);

    SampleRate rate{};
    if (!match_sample_rate(hz, rate)) {
        Log::error("Stream1090") << "Unsupported sample rate: " << raw;
        std::exit(1);
    }
    return rate;
}
