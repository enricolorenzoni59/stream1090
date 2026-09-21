#include "AdaptiveGainLogic.hpp"
#include <cstdint>

static bool expect(bool value) { return value; }

int main() {
    int modeCalls = 0;
    int gainCalls = 0;
    bool modeFails = false;
    const auto mode = [&] {
        modeCalls++;
        return modeFails ? -1 : 0;
    };
    const auto apply = [&] {
        gainCalls++;
        return 0;
    };
    if (!expect(applyManualTunerGain(mode, apply)) ||
        !expect(modeCalls == 1 && gainCalls == 1))
        return 1;
    modeFails = true;
    if (!expect(!applyManualTunerGain(mode, apply)) ||
        !expect(modeCalls == 2 && gainCalls == 1))
        return 1;

    // The 3.7 dB entry is adjacent to 2.7 dB even though a 2 dB request
    // would otherwise snap back to the current value.
    if (!expect(shadowValueNeedsApply(false, 0.0f, 0.0f)) ||
        !expect(!shadowValueNeedsApply(true, 49.6f, 49.6f)) ||
        !expect(adaptiveGainTargetIndex(4, 29, +1) == 5) ||
        !expect(adaptiveGainTargetIndex(5, 29, -1) == 4) ||
        !expect(adaptiveGainTargetIndex(5, 29, -3) == 2) ||
        !expect(adaptiveGainTargetIndex(0, 29, -1) == 0) ||
        !expect(adaptiveGainTargetIndex(28, 29, +1) == 28) ||
        !expect(adaptiveHistogramDelta(3, UINT32_MAX - 1) == 5))
        return 1;

    // A silent window is absorbed only for the cable-transient length;
    // after that the loop must decide again, and a populated window re-arms.
    {
        int consecutive = 0;
        const int threshold = 3;
        const double silentRms = 1.5;
        for (int i = 0; i < threshold; i++)
            if (!expect(advanceSilentFloor(consecutive, threshold, 0.8, 0.0, silentRms)
                        == SilentFloorState::Transient))
                return 1;
        if (!expect(advanceSilentFloor(consecutive, threshold, 0.8, 0.0, silentRms)
                    == SilentFloorState::Persistent) ||
            !expect(advanceSilentFloor(consecutive, threshold, 0.8, 0.0, silentRms)
                    == SilentFloorState::Persistent))
            return 1;
        if (!expect(advanceSilentFloor(consecutive, threshold, 4.0, 0.0, silentRms)
                    == SilentFloorState::Quiet) ||
            !expect(consecutive == 0) ||
            !expect(advanceSilentFloor(consecutive, threshold, 0.8, 0.0, silentRms)
                    == SilentFloorState::Transient))
            return 1;
        // a rail hit is a populated floor, not an open connector
        if (!expect(advanceSilentFloor(consecutive, threshold, 0.8, 0.01, silentRms)
                    == SilentFloorState::Quiet))
            return 1;
    }
    return 0;
}
