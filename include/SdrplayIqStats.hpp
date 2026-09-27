/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Owned exclusively by the Stream A callback. Fixed storage; no allocation,
// locks or per-sample atomics. Statistics describe API scalars, not ADC codes.
class SdrplayIqStats {
  public:
    static constexpr unsigned Stride = 64;
    static constexpr double DbfsFloor = -120.0;
    struct Window {
        uint64_t scalars;
        double rmsDbfs, medianDbfs, p999Dbfs, peakDbfs, noiseSigmaDbfs;
        double railFraction, nearFullFraction, centerFraction;
    };
    void reset() noexcept {
        histogram_.fill(0);
        received_ = scalars_ = squares_ = rails_ = nearFull_ = center_ = 0;
        skip_ = peak_ = 0;
    }
    template<class Publish>
    void observe(const short* i, const short* q, unsigned count, unsigned rate, Publish publish) {
        // Windows cover exactly one second of contiguous received IQ. Keep
        // the sampling phase across callbacks, including short callbacks.
        while (count) {
            const unsigned n = std::min<uint64_t>(count, rate - received_);
            for (unsigned k = skip_; k < n; k += Stride) {
                scalar(i[k]); scalar(q[k]);
            }
            skip_ = (skip_ + Stride - n % Stride) % Stride;
            received_ += n; i += n; q += n; count -= n;
            if (received_ == rate) {
                publish(window());
                reset();
            }
        }
    }
  private:
    static double dbfs(double magnitude) noexcept {
        return magnitude > 0 ? std::max(DbfsFloor, 20.0 * std::log10(magnitude / 32768.0)) : DbfsFloor;
    }
    void scalar(short sample) noexcept {
        const int value = sample;
        const unsigned amplitude = value < 0 ? unsigned(-value) : unsigned(value);
        ++histogram_[amplitude]; ++scalars_;
        squares_ += uint64_t(amplitude) * amplitude;
        peak_ = std::max(peak_, amplitude);
        rails_ += value == -32768 || value == 32767;
        nearFull_ += amplitude >= 32000;
        center_ += amplitude <= 1;
    }
    Window window() const noexcept {
        uint64_t seen = 0;
        unsigned median = 0, p999 = 0;
        const uint64_t medianRank = (scalars_ + 1) / 2;
        const uint64_t highRank = (scalars_ * 999 + 999) / 1000;
        for (unsigned bin = 0; bin <= peak_; ++bin) {
            const auto previous = seen;
            seen += histogram_[bin];
            if (previous < medianRank && seen >= medianRank) median = bin;
            if (seen >= highRank) { p999 = bin; break; }
        }
        return {scalars_, dbfs(std::sqrt(double(squares_) / scalars_)),
                dbfs(median), dbfs(p999), dbfs(peak_), dbfs(median / 0.6744897501960817),
                double(rails_) / scalars_, double(nearFull_) / scalars_, double(center_) / scalars_};
    }
    std::array<uint32_t, 32769> histogram_{};
    uint64_t received_ = 0, scalars_ = 0, squares_ = 0, rails_ = 0, nearFull_ = 0, center_ = 0;
    unsigned skip_ = 0, peak_ = 0;
};
