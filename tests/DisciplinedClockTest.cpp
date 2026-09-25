/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "devices/DisciplinedClock.hpp"

#include <cstdio>
#include <thread>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL %s\n", what);
            failures++;
        }
    };

    // It is CLOCK_MONOTONIC: read both back to back, they agree to well
    // within a millisecond.
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const auto a = DisciplinedClock::now().time_since_epoch();
    const auto mono = std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
    check(a - mono < std::chrono::milliseconds(1) && mono - a < std::chrono::milliseconds(1),
          "matches CLOCK_MONOTONIC");

    // Monotonic, and it advances with real time.
    const auto t0 = DisciplinedClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t1 = DisciplinedClock::now();
    check(t1 > t0, "advances");
    check(t1 - t0 >= std::chrono::milliseconds(45), "tracks elapsed time");

    return failures == 0 ? 0 : 1;
}
