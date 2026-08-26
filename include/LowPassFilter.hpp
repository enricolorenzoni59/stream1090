/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2025 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#pragma once

#include <iostream>
#include <numeric>
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <bit>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include "Sampler.hpp"
#include "CustomFilterTaps.hpp"

/*
 * How the filter is evaluated
 * ---------------------------
 * The output is
 *
 *     y[n] = sum_{k=0..N-1} taps[k] * x[n - (B-1) + k]      B = bit_ceil(N)
 *
 * so the taps run over B consecutive input samples ending at x[n]. Written per
 * sample against a ring buffer, every tap needs its own masked index, the whole
 * thing stays scalar, and the tap sum ends in a horizontal reduction.
 *
 * The block form flips the two loops. The samples of a block are copied behind
 * the B-1 samples of history, which makes the input contiguous, and then the
 * outer loop runs over the taps while the inner one produces four output
 * samples at a time:
 *
 *     acc[0..3] += taps[k] * w[i+k .. i+k+3]
 *
 * Four independent accumulators, one tap shared across them, and no horizontal
 * reduction at all. That inner group of four is what the vectorizer turns into
 * a single vector FMA, so the shape is left to the compiler rather than
 * written out in intrinsics.
 *
 * Summation stays in tap order, so each output accumulates its taps in the
 * same sequence the plain scalar loop would use.
 */
namespace FirDetail {

// tap count rounded up to a whole group of four
constexpr size_t padTapCount(size_t n) noexcept {
    return (n + 3u) & ~size_t(3u);
}

// taps live in Q15, samples in Q14, so a product is Q29 and a whole
// tap sum still fits an int32 as long as the taps sum to about one
inline constexpr int TapFracBits = 15;

/// Rounds a tap into Q15, saturating instead of converting out of range.
/// Without the two guards a tap of magnitude one or more overflows the
/// conversion, which is undefined: the value that comes back is whatever the
/// optimiser felt like, and it differs between targets.
constexpr int16_t toQ15(float t) noexcept {
    const float x = t * float(1 << TapFracBits);
    if (x >= 32767.0f)
        return 32767;
    if (x <= -32768.0f)
        return -32768;
    return int16_t(x >= 0.0f ? x + 0.5f : x - 0.5f);
}

// The ends of the range, and the cases that used to be undefined: anything at
// or past full scale must saturate rather than wrap.
static_assert(toQ15(0.0f) == 0);
static_assert(toQ15(1.0f) == 32767);
static_assert(toQ15(-1.0f) == -32768);
static_assert(toQ15(2.0f) == 32767);
static_assert(toQ15(-2.0f) == -32768);
static_assert(toQ15(1e9f) == 32767);
static_assert(toQ15(-1e9f) == -32768);
// Just inside full scale still rounds, and stays inside int16.
static_assert(toQ15(0.99f) == 32440);
static_assert(toQ15(-0.99f) == -32440);

/// The largest absolute tap sum the int32 accumulator can carry safely.
///
/// Each product is a Q15 tap (up to 2^15) times a Q14 sample (up to 2^14), so
/// the running sum leaves int32 once the taps sum past four. Measured, the
/// tables in the tree reach 1.62 and the filters people contribute reach 1.79;
/// after the Airspy branch alignment convolves them the worst is 1.98. Three
/// clears every one of those by half again and still stops well short of the
/// arithmetic limit.
inline constexpr float MaxTapMagnitudeSum = 3.0f;

template <typename Taps> constexpr float tapMagnitudeSum(const Taps& taps) noexcept {
    float magnitude = 0.0f;
    for (const float t : taps)
        magnitude += t < 0.0f ? -t : t;
    return magnitude;
}

/// Whether a tap set leaves the int32 accumulator room to work.
template <typename Taps> constexpr bool tapsFitAccumulator(const Taps& taps) noexcept {
    return tapMagnitudeSum(taps) <= MaxTapMagnitudeSum;
}

/// Taps that arrive at run time through -f cannot be checked at build time, and
/// the branch alignment grows their sum before the filter ever sees them, so the
/// check belongs here, where the coefficients are final. Once per tap load, off
/// the sample path.
///
/// Two ways a tap set silently stops being the filter that was asked for: a
/// coefficient past +-1 does not fit Q15 and toQ15() saturates it, and a set
/// whose absolute sum passes MaxTapMagnitudeSum overflows the accumulator. The
/// output is wrong either way and nothing downstream can tell, so this refuses
/// rather than warns. Tap sets are installed while the pipeline is built,
/// before any samples flow, so it cannot interrupt a running receiver.
inline void requireTapsFitAccumulator(const std::vector<float>& taps, const char* branch) {
    if (taps.empty())
        return;

    for (const float t : taps) {
        if (t > 1.0f || t < -1.0f) {
            std::ostringstream out;
            out << branch << " tap " << t
                << " does not fit the Q15 coefficients the filter runs on, which hold -1 to 1. It "
                   "would be clamped, and the filter would not be the one asked for.";
            throw std::invalid_argument(out.str());
        }
    }

    if (!tapsFitAccumulator(taps)) {
        std::ostringstream out;
        out << branch << " taps sum to " << tapMagnitudeSum(taps) << ", past the " << MaxTapMagnitudeSum
            << " the fixed-point accumulator can carry. Scale them down.";
        throw std::invalid_argument(out.str());
    }
}

/// Filters one contiguous block. w* hold history followed by the new
/// samples, so w*[i + k] is the k-th tap partner of output i. Symmetric
/// integer taps may pair their samples before multiplying: unlike the old
/// float path this preserves the final rounded result exactly.
template <bool Symmetric>
inline void firBlock(const int16_t* __restrict taps, size_t numTaps, const int16_t* __restrict wI,
                     const int16_t* __restrict wQ, int16_t* __restrict outI, int16_t* __restrict outQ,
                     size_t n) noexcept {
    size_t i = 0;

#if defined(__aarch64__)
    // Eight lanes first, for the same reason as IQDualLowPass: this kernel
    // is load bound, not compute bound, and twice the lanes amortises each
    // tap load and each loop iteration over twice the output. The furthest
    // element read is unchanged -- the last group starts eight earlier and
    // reaches seven further -- so the callers' padding still covers it.
    for (; i + 8 <= n; i += 8) {
        int32x4_t accI0 = vdupq_n_s32(0), accI1 = vdupq_n_s32(0);
        int32x4_t accQ0 = vdupq_n_s32(0), accQ1 = vdupq_n_s32(0);

        if constexpr (Symmetric) {
            const size_t half = numTaps / 2;
            for (size_t k = 0; k < half; ++k) {
                const size_t opposite = numTaps - 1 - k;
                const int16x8_t aI = vld1q_s16(wI + i + k);
                const int16x8_t bI = vld1q_s16(wI + i + opposite);
                const int16x8_t aQ = vld1q_s16(wQ + i + k);
                const int16x8_t bQ = vld1q_s16(wQ + i + opposite);
                accI0 = vmlaq_n_s32(accI0, vaddl_s16(vget_low_s16(aI), vget_low_s16(bI)), taps[k]);
                accI1 = vmlaq_n_s32(accI1, vaddl_high_s16(aI, bI), taps[k]);
                accQ0 = vmlaq_n_s32(accQ0, vaddl_s16(vget_low_s16(aQ), vget_low_s16(bQ)), taps[k]);
                accQ1 = vmlaq_n_s32(accQ1, vaddl_high_s16(aQ, bQ), taps[k]);
            }
            if (numTaps & 1) {
                const int16x8_t vI = vld1q_s16(wI + i + half);
                const int16x8_t vQ = vld1q_s16(wQ + i + half);
                accI0 = vmlal_n_s16(accI0, vget_low_s16(vI), taps[half]);
                accI1 = vmlal_high_n_s16(accI1, vI, taps[half]);
                accQ0 = vmlal_n_s16(accQ0, vget_low_s16(vQ), taps[half]);
                accQ1 = vmlal_high_n_s16(accQ1, vQ, taps[half]);
            }
        } else {
            for (size_t k = 0; k < numTaps; ++k) {
                const int16x8_t vI = vld1q_s16(wI + i + k);
                const int16x8_t vQ = vld1q_s16(wQ + i + k);
                accI0 = vmlal_n_s16(accI0, vget_low_s16(vI), taps[k]);
                accI1 = vmlal_high_n_s16(accI1, vI, taps[k]);
                accQ0 = vmlal_n_s16(accQ0, vget_low_s16(vQ), taps[k]);
                accQ1 = vmlal_high_n_s16(accQ1, vQ, taps[k]);
            }
        }

        const int32x4_t rounding = vdupq_n_s32(1 << (TapFracBits - 1));
        vst1q_s16(outI + i, vcombine_s16(vshrn_n_s32(vaddq_s32(accI0, rounding), TapFracBits),
                                         vshrn_n_s32(vaddq_s32(accI1, rounding), TapFracBits)));
        vst1q_s16(outQ + i, vcombine_s16(vshrn_n_s32(vaddq_s32(accQ0, rounding), TapFracBits),
                                         vshrn_n_s32(vaddq_s32(accQ1, rounding), TapFracBits)));
    }
#endif

    // The inner group of four is what the vectorizer turns into a widening
    // multiply accumulate. Four int32 accumulators to a vector, the same
    // count float had, but the operands are half the width.
    for (; i + 4 <= n; i += 4) {
#if defined(__ARM_NEON)
        int32x4_t accI = vdupq_n_s32(0);
        int32x4_t accQ = vdupq_n_s32(0);

        if constexpr (Symmetric) {
            const size_t half = numTaps / 2;
            for (size_t k = 0; k < half; ++k) {
                const size_t opposite = numTaps - 1 - k;
                const int32x4_t pairI = vaddl_s16(vld1_s16(wI + i + k), vld1_s16(wI + i + opposite));
                const int32x4_t pairQ = vaddl_s16(vld1_s16(wQ + i + k), vld1_s16(wQ + i + opposite));
                accI = vmlaq_n_s32(accI, pairI, taps[k]);
                accQ = vmlaq_n_s32(accQ, pairQ, taps[k]);
            }
            if (numTaps & 1) {
                accI = vmlal_n_s16(accI, vld1_s16(wI + i + half), taps[half]);
                accQ = vmlal_n_s16(accQ, vld1_s16(wQ + i + half), taps[half]);
            }
        } else {
            for (size_t k = 0; k < numTaps; ++k) {
                accI = vmlal_n_s16(accI, vld1_s16(wI + i + k), taps[k]);
                accQ = vmlal_n_s16(accQ, vld1_s16(wQ + i + k), taps[k]);
            }
        }

        const int32x4_t rounding = vdupq_n_s32(1 << (TapFracBits - 1));
        vst1_s16(outI + i, vshrn_n_s32(vaddq_s32(accI, rounding), TapFracBits));
        vst1_s16(outQ + i, vshrn_n_s32(vaddq_s32(accQ, rounding), TapFracBits));
#else
        int32_t accI[4] = {0, 0, 0, 0};
        int32_t accQ[4] = {0, 0, 0, 0};

        for (size_t k = 0; k < numTaps; ++k) {
            const int32_t t = taps[k];
            for (size_t l = 0; l < 4; ++l) {
                accI[l] += t * int32_t(wI[i + k + l]);
                accQ[l] += t * int32_t(wQ[i + k + l]);
            }
        }

        for (size_t l = 0; l < 4; l++) {
            outI[i + l] = int16_t((accI[l] + (1 << (TapFracBits - 1))) >> TapFracBits);
            outQ[i + l] = int16_t((accQ[l] + (1 << (TapFracBits - 1))) >> TapFracBits);
        }
#endif
    }

    // whatever does not fill a vector
    for (; i < n; i++) {
        int32_t sumI = 0;
        int32_t sumQ = 0;
        for (size_t k = 0; k < numTaps; ++k) {
            sumI += int32_t(taps[k]) * int32_t(wI[i + k]);
            sumQ += int32_t(taps[k]) * int32_t(wQ[i + k]);
        }
        outI[i] = int16_t((sumI + (1 << (TapFracBits - 1))) >> TapFracBits);
        outQ[i] = int16_t((sumQ + (1 << (TapFracBits - 1))) >> TapFracBits);
    }
}

// how many samples the block filter works on in one go
inline constexpr size_t ChunkSize = 256;

} // namespace FirDetail

template <SampleRate inputRate, SampleRate outputRate> class IQLowPass {
  public:
    IQLowPass() {
        std::fill(std::begin(m_historyI), std::end(m_historyI), int16_t(0));
        std::fill(std::begin(m_historyQ), std::end(m_historyQ), int16_t(0));
    }

    std::string toString() const {
        std::ostringstream oss;
        oss << "[IQLowPass] tap count: " << numTaps << " symmetric: " << areTapsSymmetric << "\n";
        oss << "[IQLowPass] taps: (";
        for (size_t i = 0; i < numTaps; i++) {
            oss << taps[i]; //std::bit_cast<uint32_t>(taps[i]);
            if (i + 1 < numTaps) {
                oss << ", ";
            }
        }
        oss << ")";
        return oss.str();
    }

    /// Filters a whole block in place. This is the path the input reader takes.
    void applyBlock(int16_t* __restrict I, int16_t* __restrict Q, size_t n) noexcept {
        alignas(32) int16_t workI[WorkSize];
        alignas(32) int16_t workQ[WorkSize];

        for (size_t base = 0; base < n; base += FirDetail::ChunkSize) {
            const size_t m = std::min(FirDetail::ChunkSize, n - base);

            // history, then the fresh samples
            std::copy(m_historyI, m_historyI + HistorySize, workI);
            std::copy(m_historyQ, m_historyQ + HistorySize, workQ);
            std::copy(I + base, I + base + m, workI + HistorySize);
            std::copy(Q + base, Q + base + m, workQ + HistorySize);
            // the zero taps of the last vector may reach past the samples, so
            // make sure they land on zeros and not on stack garbage
            std::fill(workI + HistorySize + m, workI + HistorySize + m + numPaddedTaps, int16_t(0));
            std::fill(workQ + HistorySize + m, workQ + HistorySize + m + numPaddedTaps, int16_t(0));

            FirDetail::firBlock<areTapsSymmetric>(paddedTaps.data(), numTaps, workI, workQ, I + base, Q + base, m);

            // the tail of the work buffer is the history of the next chunk
            std::copy(workI + m, workI + m + HistorySize, m_historyI);
            std::copy(workQ + m, workQ + m + HistorySize, m_historyQ);
        }
    }

    // single sample entry point, kept for pipelines that are driven per sample
    void apply(int16_t& value_I, int16_t& value_Q) noexcept {
        applyBlock(&value_I, &value_Q, 1);
    }

  private:
    static constexpr auto taps = LowPassTaps::getCustomTaps<inputRate, outputRate>();
    static_assert(FirDetail::tapsFitAccumulator(taps),
                  "these taps sum too far past one: the int32 accumulator in firBlock() would "
                  "overflow, see tapsFitAccumulator");
    static constexpr auto numTaps = taps.size();
    static constexpr auto bufferSize = std::bit_ceil(numTaps);
    static constexpr bool areTapsOdd = LowPassTaps::areCustomTapsOdd<inputRate, outputRate>();
    static constexpr bool areTapsSymmetric = LowPassTaps::areCustomTapsSymmetric<inputRate, outputRate>();

    static constexpr size_t numPaddedTaps = FirDetail::padTapCount(numTaps);

    // the window of an output reaches B-1 samples back
    static constexpr size_t HistorySize = bufferSize - 1;

    // taps converted to Q15 and zero padded to a whole number of vectors
    static constexpr auto paddedTaps = [] {
        std::array<int16_t, numPaddedTaps> p{};
        for (size_t i = 0; i < numTaps; i++)
            p[i] = FirDetail::toQ15(taps[i]);
        return p;
    }();

    // room for the history, a chunk of samples, and the zero padded tap tail
    static constexpr size_t WorkSize = FirDetail::ChunkSize + HistorySize + numPaddedTaps;

    alignas(32) int16_t m_historyI[HistorySize > 0 ? HistorySize : 1];
    alignas(32) int16_t m_historyQ[HistorySize > 0 ? HistorySize : 1];
};

template <size_t MaxNumTaps = 64> class IQLowPassDynamic {
  public:
    IQLowPassDynamic() : m_numTaps(1), m_areTapsSymmetric(true), m_areTapsOdd(true) {
        // set all arrays to 0.0f
        std::fill(m_taps.begin(), m_taps.end(), int16_t(0));
        std::fill(m_historyI.begin(), m_historyI.end(), int16_t(0));
        std::fill(m_historyQ.begin(), m_historyQ.end(), int16_t(0));
        // default is instant response pass through, i.e., 1 tap being 1.0f
        m_taps[0] = FirDetail::toQ15(1.0f);
    }

    IQLowPassDynamic(const std::vector<float>& taps) : IQLowPassDynamic() {
        setTaps(taps);
    }

    std::string toString() const {
        std::ostringstream oss;
        oss << "[IQLowPassDynamic] tap count: " << m_numTaps << " symmetric: " << m_areTapsSymmetric << "\n";
        oss << "[IQLowPassDynamic] taps: (";
        for (size_t i = 0; i < numTaps(); i++) {
            oss << m_taps[i]; //oss << std::bit_cast<uint32_t>(m_taps[i]);
            if ((i + 1) < numTaps()) {
                oss << ", ";
            }
        }
        oss << ")";
        return oss.str();
    }

    void printTabs() {
        std::cerr << "Sym: " << m_areTapsSymmetric << std::endl;
        std::cerr << "Odd: " << m_areTapsOdd << std::endl;
        std::cerr << "Num: " << numTaps() << std::endl;
        for (size_t i = 0; i < numTaps(); i++) {
            std::cerr << m_taps[i] << std::endl;
        }
    }

    bool setTaps(const std::vector<float>& newTaps) {
        FirDetail::requireTapsFitAccumulator(newTaps, "IQ FIR");
        if (newTaps.size() == 0)
            return false;

        if (newTaps.size() <= maxNumTaps()) {
            // copy the new values, in Q15, and clear whatever the previous taps left behind
            std::fill(m_taps.begin(), m_taps.end(), int16_t(0));
            for (size_t i = 0; i < newTaps.size(); i++)
                m_taps[i] = FirDetail::toQ15(newTaps[i]);
            // get the new number of tabs
            m_numTaps = newTaps.size();
            m_bufferSize = std::bit_ceil(m_numTaps);
            m_historySize = m_bufferSize - 1;
            // the block filter runs over whole vectors, the padding taps are zero
            m_numPaddedTaps = FirDetail::padTapCount(m_numTaps);
            // check if we have an odd number of taps
            m_areTapsOdd = (m_numTaps % 2) != 0;
            // check if they are symmetric, regardless of if the number is odd or even
            m_areTapsSymmetric = true;
            for (size_t i = 0; i < numTaps() / 2; i++) {
                // compare the opposing elements
                if (m_taps[i] != m_taps[numTaps() - 1 - i]) {
                    // one es enough to break symmetry
                    m_areTapsSymmetric = false;
                    break;
                }
            }
            // the old history was lined up for a different window length
            std::fill(m_historyI.begin(), m_historyI.end(), int16_t(0));
            std::fill(m_historyQ.begin(), m_historyQ.end(), int16_t(0));
            //printTabs();
            return true;
        }
        return false;
    }

    bool loadFromFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }

        std::vector<float> taps;
        taps.reserve(MaxNumTaps);

        std::string line;
        while (std::getline(file, line)) {
            // trim whitespace
            if (line.empty())
                continue;

            // skip comments
            if (line[0] == '#')
                continue;

            // parse float
            try {
                float v = std::stof(line);
                taps.push_back(v);
            } catch (...) {
                // malformed line
                return false;
            }

            // too many taps
            if (taps.size() > MaxNumTaps) {
                return false;
            }
        }

        // must have at least one tap
        if (taps.empty()) {
            return false;
        }
#if defined(STATS_ENABLED) && STATS_ENABLED
        std::cerr << "[Stream1090] Loaded " << taps.size() << " taps from " << filename << std::endl;
#endif
        return setTaps(taps);
    }

    // returns the maximum number of taps.
    // This is a compile time constant and is 64 by default.
    size_t maxNumTaps() const noexcept {
        return MaxNumTaps;
    }

    // returns the actual number of taps that are in use.
    size_t numTaps() const noexcept {
        return m_numTaps;
    }

    /// Filters a whole block in place.
    void applyBlock(int16_t* __restrict I, int16_t* __restrict Q, size_t n) noexcept {
        alignas(32) int16_t workI[WorkSize];
        alignas(32) int16_t workQ[WorkSize];

        for (size_t base = 0; base < n; base += FirDetail::ChunkSize) {
            const size_t m = std::min(FirDetail::ChunkSize, n - base);

            std::copy(m_historyI.begin(), m_historyI.begin() + m_historySize, workI);
            std::copy(m_historyQ.begin(), m_historyQ.begin() + m_historySize, workQ);
            std::copy(I + base, I + base + m, workI + m_historySize);
            std::copy(Q + base, Q + base + m, workQ + m_historySize);
            // keep the zero taps of the last vector on zeros, not on stack garbage
            std::fill(workI + m_historySize + m, workI + m_historySize + m + m_numPaddedTaps, int16_t(0));
            std::fill(workQ + m_historySize + m, workQ + m_historySize + m + m_numPaddedTaps, int16_t(0));

#if defined(__ARM_NEON)
            if (m_areTapsSymmetric) {
                FirDetail::firBlock<true>(m_taps.data(), m_numTaps, workI, workQ, I + base, Q + base, m);
            } else {
                FirDetail::firBlock<false>(m_taps.data(), m_numTaps, workI, workQ, I + base, Q + base, m);
            }
#else
            FirDetail::firBlock<false>(m_taps.data(), m_numTaps, workI, workQ, I + base, Q + base, m);
#endif

            std::copy(workI + m, workI + m + m_historySize, m_historyI.begin());
            std::copy(workQ + m, workQ + m + m_historySize, m_historyQ.begin());
        }
    }

    // applies the FIR to a single I and Q pair.
    void apply(int16_t& value_I, int16_t& value_Q) noexcept {
        applyBlock(&value_I, &value_Q, 1);
    }

  private:
    static constexpr size_t TapCapacity = FirDetail::padTapCount(MaxNumTaps);
    static constexpr size_t MaxHistorySize = std::bit_ceil(MaxNumTaps);
    static constexpr size_t WorkSize = FirDetail::ChunkSize + MaxHistorySize + TapCapacity;

    // the number of taps currently used
    size_t m_numTaps;

    // the same rounded up to a whole number of SIMD vectors
    // firBlock() requires a multiple of four, and the default constructed
    // filter is a one tap pass through, so start at a full group
    size_t m_numPaddedTaps = FirDetail::padTapCount(1);

    // flag indicating if the taps are symmetric
    bool m_areTapsSymmetric;

    // flag indicating if the number of taps is even or odd
    bool m_areTapsOdd;

    // size of the filter window is the smallest power of 2 with >= numTaps
    size_t m_bufferSize = 1;

    // how far back an output reaches
    size_t m_historySize = 0;

    // buffer for the taps, zero padded to a whole vector
    alignas(32) std::array<int16_t, TapCapacity> m_taps;

    // the samples that the next block still needs
    alignas(32) std::array<int16_t, MaxHistorySize> m_historyI;
    alignas(32) std::array<int16_t, MaxHistorySize> m_historyQ;
};

/// One FIR traversal with an independent symmetric response for I and Q.
/// Airspy's U16_REAL stream is paired as (x[2k], x[2k+1]), so the branches
/// need different group delays before their magnitude represents one instant.
template <size_t MaxNumTaps = 68> class IQDualLowPass {
  public:
    IQDualLowPass() {
        setBranch(m_tapsI, m_numTapsI, m_historySizeI, m_symmetricI, {1.0f});
        setBranch(m_tapsQ, m_numTapsQ, m_historySizeQ, m_symmetricQ, {1.0f});
    }
    IQDualLowPass(const std::vector<float>& tapsI, const std::vector<float>& tapsQ) : IQDualLowPass() {
        setBranch(m_tapsI, m_numTapsI, m_historySizeI, m_symmetricI, tapsI);
        setBranch(m_tapsQ, m_numTapsQ, m_historySizeQ, m_symmetricQ, tapsQ);
    }

    void apply(int16_t& I, int16_t& Q) noexcept {
        applyBlock(&I, &Q, 1);
    }

    void applyBlock(int16_t* __restrict I, int16_t* __restrict Q, size_t n) noexcept {
        alignas(32) int16_t workI[WorkSize];
        alignas(32) int16_t workQ[WorkSize];

        for (size_t base = 0; base < n; base += FirDetail::ChunkSize) {
            const size_t count = std::min(FirDetail::ChunkSize, n - base);
            std::copy(m_historyI.begin(), m_historyI.begin() + m_historySizeI, workI);
            std::copy(m_historyQ.begin(), m_historyQ.begin() + m_historySizeQ, workQ);
            std::copy(I + base, I + base + count, workI + m_historySizeI);
            std::copy(Q + base, Q + base + count, workQ + m_historySizeQ);

            filterBlock(workI, workQ, I + base, Q + base, count);

            std::copy(workI + count, workI + count + m_historySizeI, m_historyI.begin());
            std::copy(workQ + count, workQ + count + m_historySizeQ, m_historyQ.begin());
        }
    }

    std::string toString() const {
        std::ostringstream out;
        out << "[IQDualLowPass] I taps: " << m_numTapsI << " Q taps: " << m_numTapsQ;
        return out.str();
    }

  private:
    static constexpr size_t TapCapacity = FirDetail::padTapCount(MaxNumTaps);
    static constexpr size_t MaxHistorySize = std::bit_ceil(MaxNumTaps);
    static constexpr size_t WorkSize = FirDetail::ChunkSize + MaxHistorySize + TapCapacity;

    static void setBranch(std::array<int16_t, TapCapacity>& target, size_t& count, size_t& historySize, bool& symmetric,
                          const std::vector<float>& source) {
        FirDetail::requireTapsFitAccumulator(source, "the IQ FIR branch, after alignment,");
        std::fill(target.begin(), target.end(), int16_t(0));
        count = std::min(source.size(), MaxNumTaps);
        if (count == 0) {
            count = 1;
            target[0] = FirDetail::toQ15(1.0f);
        } else {
            for (size_t i = 0; i < count; ++i)
                target[i] = FirDetail::toQ15(source[i]);
        }
        historySize = std::bit_ceil(count) - 1;
        symmetric = true;
        for (size_t i = 0; i < count / 2; ++i)
            symmetric &= target[i] == target[count - 1 - i];
    }

    /// Dispatches the branch symmetry once per block. Testing it inside the
    /// loop, as this did, costs about a quarter of the kernel: the compiler
    /// cannot specialise the tap loop across a runtime branch. firBlock() takes
    /// the same flag as a template parameter for the same reason, and
    /// IQLowPassDynamic picks its instantiation outside the loop.
    void filterBlock(const int16_t* __restrict workI, const int16_t* __restrict workQ, int16_t* __restrict outI,
                     int16_t* __restrict outQ, size_t count) const noexcept {
        if (m_symmetricI) {
            if (m_symmetricQ)
                filterBlockTyped<true, true>(workI, workQ, outI, outQ, count);
            else
                filterBlockTyped<true, false>(workI, workQ, outI, outQ, count);
        } else {
            if (m_symmetricQ)
                filterBlockTyped<false, true>(workI, workQ, outI, outQ, count);
            else
                filterBlockTyped<false, false>(workI, workQ, outI, outQ, count);
        }
    }

    template <bool SymI, bool SymQ>
    void filterBlockTyped(const int16_t* __restrict workI, const int16_t* __restrict workQ, int16_t* __restrict outI,
                          int16_t* __restrict outQ, size_t count) const noexcept {
        // Both branches walk k in one loop, the way firBlock does. Two
        // independent accumulator chains in one body give the pipeline
        // something to do while each multiply-accumulate retires; run as two
        // loops they serialise on their own accumulator. The tails cover the
        // branches having different tap counts.
        // Only the vector paths below read these; a target without NEON drops
        // straight to the scalar tail, and -Wunused-variable would fire there.
        [[maybe_unused]] const size_t stepsI = SymI ? m_numTapsI / 2 : m_numTapsI;
        [[maybe_unused]] const size_t stepsQ = SymQ ? m_numTapsQ / 2 : m_numTapsQ;
        [[maybe_unused]] const size_t shared = stepsI < stepsQ ? stepsI : stepsQ;

        size_t i = 0;
#if defined(__aarch64__)
        // Eight lanes at a time. Profiling on a Cortex-A72 put 63% of this
        // kernel's cycles in the sample loads and 25% in the tap loads,
        // against 2% in the multiply-accumulate: it is load bound, and every
        // group re-reads the same tap. Twice the lanes amortises each tap load
        // and each loop iteration over twice the output.
        for (; i + 8 <= count; i += 8) {
            int32x4_t accI0 = vdupq_n_s32(0), accI1 = vdupq_n_s32(0);
            int32x4_t accQ0 = vdupq_n_s32(0), accQ1 = vdupq_n_s32(0);

            const auto stepI = [&](size_t k) {
                if constexpr (SymI) {
                    const int16x8_t a = vld1q_s16(workI + i + k);
                    const int16x8_t b = vld1q_s16(workI + i + m_numTapsI - 1 - k);
                    accI0 = vmlaq_n_s32(accI0, vaddl_s16(vget_low_s16(a), vget_low_s16(b)), m_tapsI[k]);
                    accI1 = vmlaq_n_s32(accI1, vaddl_high_s16(a, b), m_tapsI[k]);
                } else {
                    const int16x8_t v = vld1q_s16(workI + i + k);
                    accI0 = vmlal_n_s16(accI0, vget_low_s16(v), m_tapsI[k]);
                    accI1 = vmlal_high_n_s16(accI1, v, m_tapsI[k]);
                }
            };
            const auto stepQ = [&](size_t k) {
                if constexpr (SymQ) {
                    const int16x8_t a = vld1q_s16(workQ + i + k);
                    const int16x8_t b = vld1q_s16(workQ + i + m_numTapsQ - 1 - k);
                    accQ0 = vmlaq_n_s32(accQ0, vaddl_s16(vget_low_s16(a), vget_low_s16(b)), m_tapsQ[k]);
                    accQ1 = vmlaq_n_s32(accQ1, vaddl_high_s16(a, b), m_tapsQ[k]);
                } else {
                    const int16x8_t v = vld1q_s16(workQ + i + k);
                    accQ0 = vmlal_n_s16(accQ0, vget_low_s16(v), m_tapsQ[k]);
                    accQ1 = vmlal_high_n_s16(accQ1, v, m_tapsQ[k]);
                }
            };

            for (size_t k = 0; k < shared; ++k) {
                stepI(k);
                stepQ(k);
            }
            for (size_t k = shared; k < stepsI; ++k)
                stepI(k);
            for (size_t k = shared; k < stepsQ; ++k)
                stepQ(k);
            if constexpr (SymI)
                if (m_numTapsI & 1) {
                    const int16x8_t v = vld1q_s16(workI + i + stepsI);
                    accI0 = vmlal_n_s16(accI0, vget_low_s16(v), m_tapsI[stepsI]);
                    accI1 = vmlal_high_n_s16(accI1, v, m_tapsI[stepsI]);
                }
            if constexpr (SymQ)
                if (m_numTapsQ & 1) {
                    const int16x8_t v = vld1q_s16(workQ + i + stepsQ);
                    accQ0 = vmlal_n_s16(accQ0, vget_low_s16(v), m_tapsQ[stepsQ]);
                    accQ1 = vmlal_high_n_s16(accQ1, v, m_tapsQ[stepsQ]);
                }

            const auto rounding = vdupq_n_s32(1 << (FirDetail::TapFracBits - 1));
            vst1q_s16(outI + i, vcombine_s16(vshrn_n_s32(vaddq_s32(accI0, rounding), FirDetail::TapFracBits),
                                             vshrn_n_s32(vaddq_s32(accI1, rounding), FirDetail::TapFracBits)));
            vst1q_s16(outQ + i, vcombine_s16(vshrn_n_s32(vaddq_s32(accQ0, rounding), FirDetail::TapFracBits),
                                             vshrn_n_s32(vaddq_s32(accQ1, rounding), FirDetail::TapFracBits)));
        }
#endif
        for (; i + 4 <= count; i += 4) {
#if defined(__ARM_NEON)
            int32x4_t accI = vdupq_n_s32(0);
            int32x4_t accQ = vdupq_n_s32(0);

            for (size_t k = 0; k < shared; ++k) {
                if constexpr (SymI)
                    accI = vmlaq_n_s32(
                        accI, vaddl_s16(vld1_s16(workI + i + k), vld1_s16(workI + i + m_numTapsI - 1 - k)), m_tapsI[k]);
                else
                    accI = vmlal_n_s16(accI, vld1_s16(workI + i + k), m_tapsI[k]);
                if constexpr (SymQ)
                    accQ = vmlaq_n_s32(
                        accQ, vaddl_s16(vld1_s16(workQ + i + k), vld1_s16(workQ + i + m_numTapsQ - 1 - k)), m_tapsQ[k]);
                else
                    accQ = vmlal_n_s16(accQ, vld1_s16(workQ + i + k), m_tapsQ[k]);
            }
            for (size_t k = shared; k < stepsI; ++k) {
                if constexpr (SymI)
                    accI = vmlaq_n_s32(
                        accI, vaddl_s16(vld1_s16(workI + i + k), vld1_s16(workI + i + m_numTapsI - 1 - k)), m_tapsI[k]);
                else
                    accI = vmlal_n_s16(accI, vld1_s16(workI + i + k), m_tapsI[k]);
            }
            for (size_t k = shared; k < stepsQ; ++k) {
                if constexpr (SymQ)
                    accQ = vmlaq_n_s32(
                        accQ, vaddl_s16(vld1_s16(workQ + i + k), vld1_s16(workQ + i + m_numTapsQ - 1 - k)), m_tapsQ[k]);
                else
                    accQ = vmlal_n_s16(accQ, vld1_s16(workQ + i + k), m_tapsQ[k]);
            }
            if constexpr (SymI)
                if (m_numTapsI & 1)
                    accI = vmlal_n_s16(accI, vld1_s16(workI + i + stepsI), m_tapsI[stepsI]);
            if constexpr (SymQ)
                if (m_numTapsQ & 1)
                    accQ = vmlal_n_s16(accQ, vld1_s16(workQ + i + stepsQ), m_tapsQ[stepsQ]);

            const auto rounding = vdupq_n_s32(1 << (FirDetail::TapFracBits - 1));
            vst1_s16(outI + i, vshrn_n_s32(vaddq_s32(accI, rounding), FirDetail::TapFracBits));
            vst1_s16(outQ + i, vshrn_n_s32(vaddq_s32(accQ, rounding), FirDetail::TapFracBits));
#else
            int32_t accI[4] = {0, 0, 0, 0};
            int32_t accQ[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < m_numTapsI; ++k)
                for (size_t lane = 0; lane < 4; ++lane)
                    accI[lane] += int32_t(m_tapsI[k]) * workI[i + k + lane];
            for (size_t k = 0; k < m_numTapsQ; ++k)
                for (size_t lane = 0; lane < 4; ++lane)
                    accQ[lane] += int32_t(m_tapsQ[k]) * workQ[i + k + lane];
            for (size_t lane = 0; lane < 4; ++lane) {
                outI[i + lane] = int16_t((accI[lane] + (1 << 14)) >> 15);
                outQ[i + lane] = int16_t((accQ[lane] + (1 << 14)) >> 15);
            }
#endif
        }
        for (; i < count; ++i) {
            int32_t accI = 0, accQ = 0;
            for (size_t k = 0; k < m_numTapsI; ++k)
                accI += int32_t(m_tapsI[k]) * workI[i + k];
            for (size_t k = 0; k < m_numTapsQ; ++k)
                accQ += int32_t(m_tapsQ[k]) * workQ[i + k];
            outI[i] = int16_t((accI + (1 << 14)) >> 15);
            outQ[i] = int16_t((accQ + (1 << 14)) >> 15);
        }
    }

    std::array<int16_t, TapCapacity> m_tapsI{};
    std::array<int16_t, TapCapacity> m_tapsQ{};
    std::array<int16_t, MaxHistorySize> m_historyI{};
    std::array<int16_t, MaxHistorySize> m_historyQ{};
    size_t m_numTapsI = 1, m_numTapsQ = 1;
    size_t m_historySizeI = 0, m_historySizeQ = 0;
    bool m_symmetricI = true, m_symmetricQ = true;
};
