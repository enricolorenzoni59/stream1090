/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "devices/DeviceConfig.hpp"

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        failures++;
    }
}

} // namespace

int main() {
    // Tuned auto-PPM timing: the first correction lands after about two
    // minutes instead of four to five.
    {
        const AutoPpmConfig defaults;
        check(defaults.intervalSeconds == 20, "auto ppm interval default");
        check(defaults.warmupSeconds == 30, "auto ppm warmup default");
        check(defaults.samples == 5, "auto ppm samples default");
    }

    // RTL-SDR defaults: fixed gain, and the IF state that matches the rate.
    {
        const auto cfg = applyBackendDefaults(DeviceConfig{}, InputDeviceType::RTLSDR, Rate_3_2_Mhz);
        check(cfg.gainDb && *cfg.gainDb == 49.6f, "rtlsdr/3.2 gain default");
        check(cfg.tunerBandwidth && *cfg.tunerBandwidth == 2430000u, "rtlsdr/3.2 narrow bandwidth");
        check(cfg.frequencyHz == 1090000000u, "rtlsdr/frequency default");
        check(cfg.autoPpm.enabled, "rtlsdr/auto ppm on by default");
    }
    {
        // A fixed correction is a deliberate choice and suppresses the
        // automatic calibration.
        DeviceConfig input;
        input.ppm = 12;
        const auto cfg = applyBackendDefaults(input, InputDeviceType::RTLSDR, Rate_2_56_Mhz);
        check(!cfg.autoPpm.enabled, "rtlsdr/explicit ppm disables auto ppm");
    }
    {
        const auto cfg = applyBackendDefaults(DeviceConfig{}, InputDeviceType::RTLSDR, Rate_2_56_Mhz);
        check(cfg.tunerBandwidth && *cfg.tunerBandwidth == 3000000u, "rtlsdr/2.56 bandwidth default");
    }
    {
        const auto cfg = applyBackendDefaults(DeviceConfig{}, InputDeviceType::RTLSDR, Rate_2_4_Mhz);
        check(cfg.tunerBandwidth && *cfg.tunerBandwidth == 3000000u, "rtlsdr/2.4 bandwidth default");
    }

    // Explicit choices must survive the defaults, including the narrow state
    // at 3.2 Msps when the user deliberately picked another one.
    {
        DeviceConfig input;
        input.gainDb = 30.0f;
        input.tunerBandwidth = 2500000u;
        const auto cfg = applyBackendDefaults(input, InputDeviceType::RTLSDR, Rate_3_2_Mhz);
        check(cfg.gainDb && *cfg.gainDb == 30.0f, "rtlsdr/explicit gain preserved");
        check(cfg.tunerBandwidth && *cfg.tunerBandwidth == 2500000u, "rtlsdr/explicit bandwidth preserved");
    }

    // Airspy defaults: the strongest combined preset with packing on. A user
    // who already chose sensitivity or a manual stage must not get linearity
    // silently injected.
    {
        const auto cfg = applyBackendDefaults(DeviceConfig{}, InputDeviceType::AIRSPY, Rate_6_0_Mhz);
        check(cfg.linearityGain && *cfg.linearityGain == 21, "airspy/linearity default");
        check(cfg.packing, "airspy/packing default");
        check(!cfg.sensitivityGain, "airspy/no sensitivity by default");
        check(!cfg.autoPpm.enabled, "airspy/no auto ppm");
    }
    {
        DeviceConfig input;
        input.sensitivityGain = 5;
        const auto cfg = applyBackendDefaults(input, InputDeviceType::AIRSPY, Rate_6_0_Mhz);
        check(!cfg.linearityGain, "airspy/sensitivity suppresses linearity default");
    }
    {
        DeviceConfig input;
        input.lnaGain = 10;
        const auto cfg = applyBackendDefaults(input, InputDeviceType::AIRSPY, Rate_6_0_Mhz);
        check(!cfg.linearityGain, "airspy/manual stage suppresses linearity default");
    }

    // Applying the defaults twice changes nothing.
    {
        const auto once = applyBackendDefaults(DeviceConfig{}, InputDeviceType::RTLSDR, Rate_3_2_Mhz);
        const auto twice = applyBackendDefaults(once, InputDeviceType::RTLSDR, Rate_3_2_Mhz);
        check(twice.gainDb && *twice.gainDb == 49.6f, "rtlsdr/idempotent gain");
        check(twice.tunerBandwidth && *twice.tunerBandwidth == 2430000u, "rtlsdr/idempotent bandwidth");
    }

    return failures == 0 ? 0 : 1;
}
