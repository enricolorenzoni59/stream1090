/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include "Sampler.hpp"

#include <cstdint>
#include <optional>
#include <string>

// The native input backends. STREAM is not a device but stdin, NONE is only a
// placeholder for the "no backend compiled" default.
enum class InputDeviceType { STREAM, AIRSPY, RTLSDR, NONE };

// Closed-loop crystal calibration. Only the RTL-SDR backend uses it today, and
// it is off unless explicitly requested.
struct AutoPpmConfig {
    bool enabled = false;
    unsigned intervalSeconds = 20;
    unsigned warmupSeconds = 30;
    unsigned samples = 5;
    int maxStep = 20;
    int deadband = 2;
    int limit = 200;
};

// The device settings that used to live in configs/*.ini, now typed. Every
// field that the user does not have to set is optional, so "left unset" is
// distinguishable from "explicitly set to the default value" and the defaults
// below can fill only the holes. Precedence is: defaults < command line.
struct DeviceConfig {
    uint32_t frequencyHz = 1090000000;
    bool biasTee = false;
    std::optional<std::string> serial;

    // RTL-SDR
    bool agc = false;
    std::optional<float> gainDb;
    std::optional<uint32_t> tunerBandwidth;
    bool offsetTuning = false;
    AutoPpmConfig autoPpm;

    // Airspy
    bool packing = true;

    // Manual per-stage gain, both backends.
    std::optional<int> lnaGain;
    std::optional<int> mixerGain;
    std::optional<int> vgaGain;
    // Airspy combined gain presets.
    std::optional<int> linearityGain;
    std::optional<int> sensitivityGain;

    std::optional<int> ppm;
};

// Fills the fields the user left unset with the automatic choice for the
// selected backend and sample rate. It only writes unset fields, so calling it
// after an explicit override is harmless.
//
// The RTL-SDR choices come from paired captures: a fixed 49.6 dB tuner gain,
// the 6 MHz IF state at 2.4/2.56 Msps and the narrow 2.43 MHz state at 3.2
// Msps where the automatic 6 MHz state aliases noise across Nyquist. Airspy
// starts from the strongest combined preset with packing on.
//
// Automatic PPM calibration is on by default for RTL-SDR: cheap crystals sit
// tens of ppm off and ADS-B is sensitive to that. An explicit ppm is a fixed
// choice and suppresses it; the caller can still override autoPpm afterwards
// for --auto-ppm / --no-auto-ppm.
inline DeviceConfig applyBackendDefaults(DeviceConfig cfg, InputDeviceType type, SampleRate inputRate) {
    if (type == InputDeviceType::RTLSDR) {
        if (!cfg.gainDb)
            cfg.gainDb = 49.6f;
        if (!cfg.tunerBandwidth)
            cfg.tunerBandwidth = (inputRate == Rate_3_2_Mhz) ? 2430000u : 3000000u;
        if (!cfg.ppm)
            cfg.autoPpm.enabled = true;
    } else if (type == InputDeviceType::AIRSPY) {
        const bool manualStages = cfg.lnaGain || cfg.mixerGain || cfg.vgaGain;
        if (!cfg.linearityGain && !cfg.sensitivityGain && !manualStages)
            cfg.linearityGain = 21;
    }
    return cfg;
}
