/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <utility>
#include <vector>

#include "Sampler.hpp"
#include "RawInputFormat.hpp"
#include "IQPipeline.hpp"
#include "LowPassFilter.hpp"

enum class IQPipelineOptions { NONE, IQ_FIR, IQ_FIR_FILE, IQ_FIR_RTL_SDR, IQ_FIR_RTL_SDR_FILE };

template <typename RawFormat, typename Sampler, IQPipelineOptions Opt> struct Preset {
    using RawFormatType = RawFormat;
    using SamplerType = Sampler;
    using RawType = typename RawFormat::RawType;

    static constexpr SampleRate inputRate = SamplerType::InputSampleRate;
    static constexpr SampleRate outputRate = SamplerType::OutputSampleRate;
    static constexpr IQPipelineOptions pipelineOption = Opt;
};

#if defined(STREAM1090_CUSTOM_INPUT) && STREAM1090_CUSTOM_INPUT
constexpr auto presets = std::make_tuple(
    // Custom Input
    Preset<IQ_FLOAT32, Sampler_2_0_to_2_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_FLOAT32, Sampler_2_0_to_4_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_FLOAT32, Sampler_2_0_to_8_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_FLOAT32, Sampler_4_0_to_4_0_Mhz, IQPipelineOptions::NONE>{});
#else
constexpr auto presets = std::make_tuple(
    // RTL-SDR (uint8) default presets
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_4_to_8_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_4_to_8_0_Mhz, IQPipelineOptions::IQ_FIR_RTL_SDR>{},
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_4_to_8_0_Mhz, IQPipelineOptions::IQ_FIR_RTL_SDR_FILE>{},

    Preset<IQ_UINT8_RTL_SDR, Sampler_2_4_to_12_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_4_to_12_0_Mhz, IQPipelineOptions::IQ_FIR_RTL_SDR>{},
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_4_to_12_0_Mhz, IQPipelineOptions::IQ_FIR_RTL_SDR_FILE>{},

    Preset<IQ_UINT8_RTL_SDR, Sampler_2_56_to_8_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_56_to_8_0_Mhz, IQPipelineOptions::IQ_FIR_RTL_SDR>{},
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_56_to_8_0_Mhz, IQPipelineOptions::IQ_FIR_RTL_SDR_FILE>{},

    Preset<IQ_UINT8_RTL_SDR, Sampler_2_56_to_12_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_56_to_12_0_Mhz, IQPipelineOptions::IQ_FIR_RTL_SDR>{},
    Preset<IQ_UINT8_RTL_SDR, Sampler_2_56_to_12_0_Mhz, IQPipelineOptions::IQ_FIR_RTL_SDR_FILE>{},

    // Airspy (uint16) default presets
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_6_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_6_0_Mhz, IQPipelineOptions::IQ_FIR>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_6_0_Mhz, IQPipelineOptions::IQ_FIR_FILE>{},

    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_12_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_12_0_Mhz, IQPipelineOptions::IQ_FIR>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_12_0_Mhz, IQPipelineOptions::IQ_FIR_FILE>{},

    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_24_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_24_0_Mhz, IQPipelineOptions::IQ_FIR>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_6_0_to_24_0_Mhz, IQPipelineOptions::IQ_FIR_FILE>{},

    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_10_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_10_0_Mhz, IQPipelineOptions::IQ_FIR>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_10_0_Mhz, IQPipelineOptions::IQ_FIR_FILE>{},

    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_24_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_24_0_Mhz, IQPipelineOptions::IQ_FIR>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_24_0_Mhz, IQPipelineOptions::IQ_FIR_FILE>{}
#if defined(STREAM1090_TOO_MUCH_CPU) && STREAM1090_TOO_MUCH_CPU
    ,
    // too much cpu samplers
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_40_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_40_0_Mhz, IQPipelineOptions::IQ_FIR>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_40_0_Mhz, IQPipelineOptions::IQ_FIR_FILE>{},

    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_48_0_Mhz, IQPipelineOptions::NONE>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_48_0_Mhz, IQPipelineOptions::IQ_FIR>{},
    Preset<IQ_UINT16_RAW_AIRSPY, Sampler_10_0_to_48_0_Mhz, IQPipelineOptions::IQ_FIR_FILE>{}
#endif
);

#endif

template <SampleRate In, SampleRate Out, IQPipelineOptions sel> struct IQPipelineSelector {
    static auto make(const std::vector<float>&) {
        return make_pipeline();
    }
};

/// Every symmetric four-tap filter [a, 0.5 - a, 0.5 - a, a] has unit DC gain
/// and a group delay of exactly 1.5 samples, whatever a is: the half sample
/// the alignment needs is not up for negotiation, and a only shapes the
/// amplitude response. a = -0.0625 is the maximally flat Lagrange choice and
/// the default. The two rates below use a value picked by sweeping a on one
/// capture window and confirmed on windows held back from that sweep; each is
/// worth a few tenths of a percent over Lagrange, and Lagrange alone already
/// carries almost all of the gain.
template <SampleRate In> constexpr float iqAlignmentEdgeTap() {
    if constexpr (In == Rate_6_0_Mhz)
        return -0.225f;
    if constexpr (In == Rate_10_0_Mhz)
        return -0.0375f;
    return -0.0625f;
}

// The FIR's effective delay is its group delay plus (bit_ceil(n) - n), because
// the history it carries is sized to the next power of two rather than to the
// tap count. tapsI and tapsQ differ by one tap, so they normally sit in the
// same bucket and that term cancels to leave the intended half sample. When a
// base length puts them either side of a power of two it does not cancel, and
// the branches end up tens of samples apart instead of half of one -- silently,
// because nothing else about the filter looks wrong. Base lengths 2, 6, 14, 30
// and 62 straddle; 27 (every rate today) and 15 do not.
constexpr bool iqAlignmentBucketsAgree(size_t baseTaps) {
    return std::bit_ceil(baseTaps + 3) == std::bit_ceil(baseTaps + 2);
}

template <SampleRate In>
std::pair<std::vector<float>, std::vector<float>> makeAlignedIQTaps(const std::vector<float>& base) {
    const float edge = iqAlignmentEdgeTap<In>();
    const std::array<float, 4> fractionalDelay{edge, 0.5f - edge, 0.5f - edge, edge};
    std::vector<float> tapsI(base.size() + fractionalDelay.size() - 1, 0.0f);
    for (size_t i = 0; i < base.size(); ++i)
        for (size_t k = 0; k < fractionalDelay.size(); ++k)
            tapsI[i + k] += base[i] * fractionalDelay[k];

    std::vector<float> tapsQ(base.size() + 2, 0.0f);
    std::copy(base.begin(), base.end(), tapsQ.begin() + 1);

    // Runtime taps come from -f, so this cannot be a static_assert. Degrade to
    // the unaligned pair rather than ship a decoder whose branches are 30
    // samples apart.
    if (!iqAlignmentBucketsAgree(base.size())) {
        std::cerr << "[Stream1090] IQ alignment disabled: a " << base.size()
                  << " tap filter straddles a power of two and would misalign "
                     "the branches\n";
        return {base, base};
    }
    return {std::move(tapsI), std::move(tapsQ)};
}

template <SampleRate In, SampleRate Out> struct IQPipelineSelector<In, Out, IQPipelineOptions::IQ_FIR> {
    static auto make(const std::vector<float>&) {
        constexpr auto source = LowPassTaps::getCustomTaps<In, Out>();
        static_assert(iqAlignmentBucketsAgree(source.size()), "built-in tap count straddles a power of two: the I/Q "
                                                              "branches would misalign, see iqAlignmentBucketsAgree");
        const std::vector<float> base(source.begin(), source.end());
        auto [tapsI, tapsQ] = makeAlignedIQTaps<In>(base);
        return make_pipeline(DCRemoval(), FlipSigns(), IQDualLowPass(tapsI, tapsQ));
    }
};

template <SampleRate In, SampleRate Out> struct IQPipelineSelector<In, Out, IQPipelineOptions::IQ_FIR_FILE> {
    static auto make(const std::vector<float>& taps) {
        auto [tapsI, tapsQ] = makeAlignedIQTaps<In>(taps);
        return make_pipeline(DCRemoval(), FlipSigns(), IQDualLowPass(tapsI, tapsQ));
    }
};

template <SampleRate In, SampleRate Out> struct IQPipelineSelector<In, Out, IQPipelineOptions::IQ_FIR_RTL_SDR> {
    static auto make(const std::vector<float>&) {
        return make_pipeline(IQLowPass<In, Out>());
    }
};

template <SampleRate In, SampleRate Out> struct IQPipelineSelector<In, Out, IQPipelineOptions::IQ_FIR_RTL_SDR_FILE> {
    static auto make(const std::vector<float>& taps) {
        return make_pipeline(IQLowPassDynamic(taps));
    }
};
