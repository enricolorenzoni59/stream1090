/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <chrono>
#include <ctime>

// The host clock the RTL-SDR crystal is measured against by the automatic PPM
// calibration. It has to carry the NTP frequency correction, or the result is
// the dongle's error relative to the computer's own oscillator rather than to
// real time. std::chrono::steady_clock does not on macOS: libc++ reads
// CLOCK_MONOTONIC_RAW there, the bare oscillator, measured 1.33 ppm off
// CLOCK_MONOTONIC on an M1 Pro. CLOCK_MONOTONIC carries the correction on both
// macOS and Linux (where it is what steady_clock already uses) and never steps.
struct DisciplinedClock {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<DisciplinedClock>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return time_point(std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec));
    }
};
