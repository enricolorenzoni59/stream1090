/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

template<typename T>
inline bool shadowValueNeedsApply(bool known, const T& shadow, const T& requested) {
    return !known || shadow != requested;
}

// An empty analog floor (rms below the silent threshold with no rail hits)
// is either the single window that catches a cable being pulled or a feed
// that is genuinely disconnected. The first is absorbed, the second is not:
// after `threshold` consecutive silent windows the loop must decide again,
// or the gain freezes at the value it had when the antenna came off.
enum class SilentFloorState {
    Quiet,       // the floor is populated
    Transient,   // absorb this window and decide nothing
    Persistent   // the silence outlasted the cable transient
};

inline SilentFloorState advanceSilentFloor(int& consecutive, int threshold,
                                           double rms, double sat, double silentRms) {
    if (!(rms < silentRms && sat <= 0.0)) {
        consecutive = 0;
        return SilentFloorState::Quiet;
    }
    if (++consecutive <= threshold)
        return SilentFloorState::Transient;
    return SilentFloorState::Persistent;
}

template<typename SetMode, typename SetGain>
inline bool applyManualTunerGain(SetMode&& setMode, SetGain&& setGain) {
    return setMode() == 0 && setGain() == 0;
}

// Move through the driver's actual gain table. The table is ordered by the
// backend, so this also handles adjacent entries whose dB delta is below 2.
inline int adaptiveGainTargetIndex(int currentIndex, int count, int direction) {
    if (count <= 0 || currentIndex < 0 || currentIndex >= count || direction == 0)
        return currentIndex;
    if (direction > 0)
        return std::min(currentIndex + 1, count - 1);
    const int positions = direction <= -3 ? 3 : 1;
    return std::max(currentIndex - std::min(positions, currentIndex), 0);
}

// The acceptance band is wider than one gain step, so a loop that stops at
// the first in-band window parks wherever it happened to cross the edge.
// Measured 2026-09-22 with two receivers on one antenna through a splitter:
// their floors sat 0.045 LSB apart at the 2.5 edge, one crossed from below
// and took a whole step to 3.57, the other stopped at 2.56, and both windows
// read "in band" for the rest of the run, 2.9 dB apart. Aim at a target and
// take the step only while it lands nearer to it, in dB, without trading one
// edge of the band for the other.
//
// How much this is worth, measured rather than assumed. A deliberate VGA
// sweep on 2026-09-22, one receiver swept and the other pinned so the traffic
// divides out, four passes in alternating order, 208k messages:
//
//   analog above digital   2.2    5.5    8.8   12.4   15.9   19.5   22.8 dB
//   yield vs the best     -1.10  -0.28  -0.34  -0.08  -0.21   0.00  -0.05 dB
//
// There is no optimum to aim at. From 5.5 dB upwards the yield is flat within
// 0.34 dB, which is the size of the pass-to-pass scatter; only the bottom of
// the range is distinguishable, and it costs about 1 dB. A single pass looked
// like it peaked at 12.4 dB - it did not, that was one pass of noise.
//
// So the setpoint wants a floor, not a target, and the low edge in the loop
// (2.5 LSB, about 9.3 dB above the digital floor) is some 4 dB more cautious
// than the measurement requires. What this change earns is therefore not
// messages but reproducibility: two receivers, or one receiver on two days,
// come to rest at the same operating point instead of anywhere inside a 6 dB
// band, which is the precondition for comparing anything at all.
//
// stepDb is what the previous climb actually did, not what it asked for: the
// quantisation share of the floor does not scale with gain (measured the same
// night, 10.2 dB of VGA lifted the floor 8.4 dB).
inline bool climbClosesOnTarget(double floorNow, double target,
                                double ceiling, double stepDb) {
    if (!(floorNow > 0.0) || !(target > 0.0) || !(stepDb > 0.0))
        return false;
    const double after = floorNow * std::pow(10.0, stepDb / 20.0);
    if (after >= ceiling)
        return false;
    return std::fabs(20.0 * std::log10(after / target))
         < std::fabs(20.0 * std::log10(floorNow / target));
}

inline uint64_t adaptiveHistogramDelta(uint32_t current, uint32_t previous) {
    return current >= previous
        ? static_cast<uint64_t>(current - previous)
        : static_cast<uint64_t>(UINT32_MAX - previous) + 1u + current;
}
