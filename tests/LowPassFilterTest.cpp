#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "LowPassFilter.hpp"

namespace {

template <size_t NumTaps> bool symmetricMatchesPlain(const std::array<int16_t, NumTaps>& taps) {
    constexpr size_t Count = 257;
    std::array<int16_t, Count + NumTaps> inputI{};
    std::array<int16_t, Count + NumTaps> inputQ{};
    std::array<int16_t, Count> plainI{};
    std::array<int16_t, Count> plainQ{};
    std::array<int16_t, Count> symmetricI{};
    std::array<int16_t, Count> symmetricQ{};

    uint32_t state = 0x13579bdu;
    for (size_t i = 0; i < inputI.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        inputI[i] = int16_t((state >> 17) - 16384);
        state = state * 1664525u + 1013904223u;
        inputQ[i] = int16_t((state >> 17) - 16384);
    }

    FirDetail::firBlock<false>(taps.data(), taps.size(), inputI.data(), inputQ.data(), plainI.data(), plainQ.data(),
                               Count);
    FirDetail::firBlock<true>(taps.data(), taps.size(), inputI.data(), inputQ.data(), symmetricI.data(),
                              symmetricQ.data(), Count);
    return plainI == symmetricI && plainQ == symmetricQ;
}

template <SampleRate InputRate, SampleRate OutputRate> bool builtInSymmetricMatchesPlain() {
    constexpr auto source = LowPassTaps::getCustomTaps<InputRate, OutputRate>();
    constexpr auto taps = [source] {
        std::array<int16_t, source.size()> result{};
        for (size_t i = 0; i < source.size(); ++i)
            result[i] = FirDetail::toQ15(source[i]);
        return result;
    }();
    static_assert(LowPassTaps::areCustomTapsSymmetric<InputRate, OutputRate>());
    return symmetricMatchesPlain(taps);
}

bool dualFilterMatchesTwoIndependentFilters() {
    const std::vector<float> tapsI{-0.125f, 0.625f, 0.625f, -0.125f};
    const std::vector<float> tapsQ{0.0f, 0.25f, 0.5f, 0.25f, 0.0f};
    IQDualLowPass dual(tapsI, tapsQ);
    IQLowPassDynamic<> referenceI(tapsI), referenceQ(tapsQ);

    constexpr size_t Count = 777;
    std::array<int16_t, Count> inputI{}, inputQ{};
    uint32_t state = 0x2468aceu;
    for (size_t i = 0; i < Count; ++i) {
        state = state * 1664525u + 1013904223u;
        inputI[i] = int16_t((state >> 17) - 16384);
        state = state * 1664525u + 1013904223u;
        inputQ[i] = int16_t((state >> 17) - 16384);
    }

    auto actualI = inputI, actualQ = inputQ;
    auto expectedI = inputI, expectedQ = inputQ;
    auto discardedI = inputI, discardedQ = inputQ;
    for (const auto& [offset, count] : {std::pair<size_t, size_t>{0, 113}, {113, 511}, {624, 153}}) {
        dual.applyBlock(actualI.data() + offset, actualQ.data() + offset, count);
        referenceI.applyBlock(expectedI.data() + offset, discardedQ.data() + offset, count);
        referenceQ.applyBlock(discardedI.data() + offset, expectedQ.data() + offset, count);
    }
    return actualI == expectedI && actualQ == expectedQ;
}

// Direct convolution in Q15, written independently of the filter classes, so
// it stays a valid oracle above 64 taps where IQLowPassDynamic's own capacity
// stops. A -f file of 64 taps plus the alignment reaches 67, so that region is
// production-reachable, and the branch for asymmetric taps has no other cover.
bool dualFilterMatchesDirectConvolution() {
    auto reference = [](const std::vector<float>& taps, size_t historySize, const std::vector<int16_t>& in) {
        std::vector<int16_t> q15;
        for (float t : taps)
            q15.push_back(FirDetail::toQ15(t));
        std::vector<int16_t> work(historySize, 0);
        work.insert(work.end(), in.begin(), in.end());
        std::vector<int16_t> out(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            int32_t acc = 0;
            for (size_t k = 0; k < q15.size(); ++k)
                acc += int32_t(q15[k]) * int32_t(work[i + k]);
            out[i] = int16_t((acc + (1 << 14)) >> 15);
        }
        return out;
    };

    uint32_t state = 0x51ed270bu;
    auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state;
    };

    for (size_t numTaps = 1; numTaps <= 68; ++numTaps) {
        for (int shape = 0; shape < 4; ++shape) {
            auto makeTaps = [&](size_t n, bool symmetric) {
                std::vector<float> t(n);
                for (size_t i = 0; i < n; ++i)
                    t[i] = float(int32_t(next() >> 20) - 512) / 1024.0f;
                if (symmetric)
                    for (size_t i = 0; i < n / 2; ++i)
                        t[n - 1 - i] = t[i];
                // firBlock() documents its accumulator as fitting an int32 only
                // while the taps sum to about one. Random coefficients over 68
                // taps blow past that, and the overflow is signed: the vector
                // path wraps in hardware but the portable path is undefined, so
                // an unnormalised filter tests the compiler, not the kernel.
                float magnitude = 0.0f;
                for (float v : t)
                    magnitude += std::abs(v);
                if (magnitude > 1.0f)
                    for (float& v : t)
                        v /= magnitude;
                return t;
            };
            const size_t otherTaps = numTaps > 1 ? numTaps - 1 : 1;
            const auto tapsI = makeTaps(numTaps, (shape & 1) != 0);
            const auto tapsQ = makeTaps(otherTaps, (shape & 2) != 0);

            constexpr size_t Count = 521;
            std::vector<int16_t> inI(Count), inQ(Count);
            for (size_t i = 0; i < Count; ++i) {
                inI[i] = int16_t((next() >> 17) - 16384);
                inQ[i] = int16_t((next() >> 17) - 16384);
            }

            // The filter carries a history sized to the next power of two, not
            // to the tap count, so the oracle has to pad the same way.
            const auto expectedI = reference(tapsI, std::bit_ceil(numTaps) - 1, inI);
            const auto expectedQ = reference(tapsQ, std::bit_ceil(otherTaps) - 1, inQ);

            IQDualLowPass<> dual(tapsI, tapsQ);
            auto actualI = inI, actualQ = inQ;
            for (size_t offset = 0; offset < Count;) {
                const size_t count = std::min(Count - offset, size_t(1 + (next() % 200)));
                dual.applyBlock(actualI.data() + offset, actualQ.data() + offset, count);
                offset += count;
            }
            if (actualI != expectedI || actualQ != expectedQ)
                return false;
        }
    }
    return true;
}

} // namespace

int main() {
    constexpr std::array<int16_t, 15> oddTaps{-81,  -66,  781,  1019, 1741, 3178, 1865, 15891,
                                              1865, 3178, 1741, 1019, 781,  -66,  -81};
    constexpr std::array<int16_t, 6> evenTaps{328, 655, 1311, 1311, 655, 328};

    if (!symmetricMatchesPlain(oddTaps) || !symmetricMatchesPlain(evenTaps) ||
        !builtInSymmetricMatchesPlain<Rate_2_4_Mhz, Rate_8_0_Mhz>() ||
        !builtInSymmetricMatchesPlain<Rate_6_0_Mhz, Rate_24_0_Mhz>() ||
        !builtInSymmetricMatchesPlain<Rate_10_0_Mhz, Rate_24_0_Mhz>() || !dualFilterMatchesTwoIndependentFilters() ||
        !dualFilterMatchesDirectConvolution()) {
        std::cerr << "Symmetric FIR changed fixed-point output\n";
        return 1;
    }
    return 0;
}
