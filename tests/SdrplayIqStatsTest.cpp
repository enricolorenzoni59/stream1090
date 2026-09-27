/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "SdrplayIqStats.hpp"
#include <stdexcept>
#include <vector>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(0)
int main() {
    SdrplayIqStats stats;
    std::vector<short> i(640, 1024), q(640, -1024);
    unsigned windows = 0;
    auto constant = [&](const SdrplayIqStats::Window& w) {
        ++windows;
        CHECK(w.scalars == 20);
        CHECK(std::fabs(w.rmsDbfs - 20*std::log10(1.0/32)) < 1e-9);
        CHECK(w.medianDbfs == w.peakDbfs && w.p999Dbfs == w.peakDbfs);
        CHECK(w.railFraction == 0 && w.nearFullFraction == 0 && w.centerFraction == 0);
    };
    // Deliberately split before/after stride boundaries. Results must be
    // independent of the SDK callback's packet size.
    for (unsigned n = 0; n < 640; ++n) stats.observe(&i[n], &q[n], 1, 640, constant);
    CHECK(windows == 1);
    stats.observe(i.data(), q.data(), 640, 640, constant);
    CHECK(windows == 2);
    i.assign(640, 0); q.assign(640, 0);
    stats.observe(i.data(), q.data(), 320, 640, constant);
    stats.reset(); // gap discards the partial window
    stats.observe(i.data(), q.data(), 320, 640, constant);
    CHECK(windows == 2);
    stats.observe(i.data(), q.data(), 320, 640, [&](const SdrplayIqStats::Window& w) {
        ++windows;
        CHECK(w.rmsDbfs == -120 && w.noiseSigmaDbfs == -120);
        CHECK(w.centerFraction == 1 && w.railFraction == 0);
    });
    CHECK(windows == 3);
    i.assign(640, -32768); q.assign(640, 32767);
    stats.observe(i.data(), q.data(), 640, 640, [&](const SdrplayIqStats::Window& w) {
        CHECK(w.railFraction == 1 && w.nearFullFraction == 1);
        CHECK(w.peakDbfs == 0 && w.rmsDbfs < 0 && w.rmsDbfs > -0.001);
    });
    // Quantiles differ from the maximum for a rare outlier.
    i.assign(64000, 100); q.assign(64000, -100); i[0] = 32000;
    stats.observe(i.data(), q.data(), 64000, 64000, [&](const SdrplayIqStats::Window& w) {
        CHECK(w.scalars == 2000);
        CHECK(w.p999Dbfs == w.medianDbfs && w.peakDbfs > w.p999Dbfs);
        CHECK(w.nearFullFraction == 0.0005 && w.railFraction == 0);
    });
}
