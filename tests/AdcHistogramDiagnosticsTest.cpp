#include "AdcHistogramDiagnostics.hpp"
#include <cmath>

int main() {
    uint8_t iq[16]{};
    for (int i = 0; i < 16; i += 2) {
        iq[i] = 100;
        iq[i + 1] = 200;
    }
    uint32_t sampled[256]{};
    if (accumulateSampledAdcHistogram(iq, 16, sampled) != 4 ||
        sampled[100] != 2 || sampled[200] != 2)
        return 5;

    uint64_t empty[256]{};
    if (summarizeAdcHistogram(empty).samples != 0)
        return 1;

    uint64_t centered[256]{};
    centered[128] = 1000;
    auto quiet = summarizeAdcHistogram(centered);
    if (quiet.samples != 1000 || quiet.bins[8] != 1000 ||
        quiet.centerPercent != 100 || quiet.railPercent != 0 ||
        quiet.absP99Lsb != 0 || quiet.absP999Lsb != 0)
        return 2;

    uint64_t clipped[256]{};
    clipped[0] = 500;
    clipped[255] = 500;
    auto loud = summarizeAdcHistogram(clipped);
    if (loud.bins[0] != 500 || loud.bins[15] != 500 ||
        loud.railPercent != 100 || loud.absP99Lsb != 128 ||
        loud.absP999Lsb != 128 || std::abs(loud.rmsLsb - 127.5) > 0.01)
        return 3;

    uint64_t rareTail[256]{};
    rareTail[128] = 990;
    rareTail[160] = 9;
    rareTail[255] = 1;
    auto mixed = summarizeAdcHistogram(rareTail);
    if (mixed.absP99Lsb != 0 || mixed.absP999Lsb != 32 ||
        mixed.railPercent != 0.1)
        return 4;
    return 0;
}
