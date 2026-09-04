/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <cstdint>

template<typename T>
inline bool shadowValueNeedsApply(bool known, const T& shadow, const T& requested) {
    return !known || shadow != requested;
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

inline uint64_t adaptiveHistogramDelta(uint32_t current, uint32_t previous) {
    return current >= previous
        ? static_cast<uint64_t>(current - previous)
        : static_cast<uint64_t>(UINT32_MAX - previous) + 1u + current;
}
