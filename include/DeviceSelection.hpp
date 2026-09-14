/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include "Cli.hpp"
#include "Global.hpp"
#include "Logger.hpp"
#include "RateUtils.hpp"
#include "devices/DeviceConfig.hpp"
#include "devices/DeviceEnumerator.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Decides whether stdin is a real input stream or just the empty default of a
// non-interactive launcher. systemd hands out /dev/null; a regular file, pipe,
// socket or real character device is an input. A pipe that has not produced
// data yet still has a producer attached, so it is never abandoned for a
// device.
enum class StdinKind { Data, NoData };

inline StdinKind classify_stdin() {
    if (isatty(STDIN_FILENO))
        return StdinKind::NoData;

    struct stat input {};
    if (fstat(STDIN_FILENO, &input) != 0)
        return StdinKind::NoData;

    struct stat nullDevice {};
    if (S_ISCHR(input.st_mode) && stat("/dev/null", &nullDevice) == 0 &&
        input.st_rdev == nullDevice.st_rdev)
        return StdinKind::NoData;

    return StdinKind::Data;
}

// The backend and the ordered serial candidates to try.
struct DeviceChoice {
    InputDeviceType type = InputDeviceType::STREAM;
    std::vector<std::string> serials;
};

// The default input rate for a backend. RTL-SDR is fixed at 2.56 Msps; the
// Airspy default is the highest rate the device reports that stream1090 also
// has a preset for (10 Msps on an R2, 6 on a Mini).
inline std::optional<SampleRate> default_input_rate(InputDeviceType type, const std::vector<std::string>& serials) {
    if (type == InputDeviceType::RTLSDR)
        return Rate_2_56_Mhz;
    if (type == InputDeviceType::AIRSPY) {
        std::optional<SampleRate> best;
        for (const auto& serial : serials) {
            for (uint32_t hz : queryAirspyRates(serial)) {
                SampleRate rate{};
                if (!match_sample_rate(static_cast<int>(hz), rate) || !has_input_rate(rate))
                    continue;
                if (!best || (int)rate > (int)*best)
                    best = rate;
            }
            if (best)
                break;
        }
        return best;
    }
    return std::nullopt;
}

// Decides where IQ comes from. An explicit --device wins; otherwise a piped
// stdin is used and an empty one (TTY or /dev/null) triggers auto detection.
// Prints the reason and returns nullopt on failure. `quiet` is for the
// recovery retries, where "no device yet" repeats every second.
inline std::optional<DeviceChoice> choose_device(const CliArgs& args, bool quiet = false) {
    DeviceChoice choice;
    const std::string kind = args.device;

    if (kind.empty() || kind == "stdin" || kind == "auto") {
        const bool isAuto = (kind == "auto") || (kind.empty() && classify_stdin() == StdinKind::NoData);
        if (!isAuto) {
            choice.type = InputDeviceType::STREAM;
            return choice;
        }
    } else if (kind != "airspy" && kind != "rtlsdr") {
        if (!quiet)
            Log::error("Stream1090") << "Unknown --device value: " << kind
                                     << " (expected stdin, auto, airspy or rtlsdr)";
        return std::nullopt;
    }

    const auto devices = enumerateDevices();

    InputDeviceType type = InputDeviceType::NONE;
    if (kind == "airspy" || kind == "rtlsdr") {
        type = (kind == "airspy") ? InputDeviceType::AIRSPY : InputDeviceType::RTLSDR;
    } else if (!devices.empty()) {
        type = devices.front().type;
    }

    if (type == InputDeviceType::NONE) {
        if (!quiet)
            Log::error("Stream1090") << "No SDR device found. Use --device stdin to read IQ from standard input.";
        return std::nullopt;
    }
    if (type == InputDeviceType::AIRSPY && !GlobalOptions::NativeAirspySupport) {
        if (!quiet)
            Log::error("Stream1090")
                << "This build has no Airspy support; rerun with --device stdin or a backend-enabled build.";
        return std::nullopt;
    }
    if (type == InputDeviceType::RTLSDR && !GlobalOptions::NativeRtlSdrSupport) {
        if (!quiet)
            Log::error("Stream1090")
                << "This build has no RTL-SDR support; rerun with --device stdin or a backend-enabled build.";
        return std::nullopt;
    }

    if (!args.serial.empty()) {
        choice.serials.push_back(args.serial);
    } else {
        for (const auto& device : devices) {
            if (device.type == type)
                choice.serials.push_back(device.serial);
        }
        if (choice.serials.empty()) {
            if (!quiet)
                Log::error("Stream1090") << "No " << (type == InputDeviceType::AIRSPY ? "Airspy" : "RTL-SDR")
                                         << " device found. Use --device stdin to read IQ from standard input.";
            return std::nullopt;
        }
    }

    choice.type = type;
    return choice;
}

// Resolves the input rate for a backend: the explicit -s, or the backend
// default. Rejects a rate that belongs to the other backend. Prints the reason
// and returns nullopt on failure.
inline std::optional<SampleRate> resolve_input_rate(const CliArgs& args, InputDeviceType type,
                                                    const std::vector<std::string>& serials) {
    SampleRate rate{};
    if (args.sampleRate.empty()) {
        auto def = default_input_rate(type, serials);
        if (!def) {
            Log::error("Stream1090") << "Could not determine a default sample rate for this device; pass -s.";
            return std::nullopt;
        }
        rate = *def;
        Log::msg("Stream1090") << "Auto-selected input rate: " << rate_mhz(rate) << " MHz";
    } else {
        rate = parse_sample_rate(args.sampleRate);
    }

    const bool airspyRate = is_airspy_rate(rate);
    if (type == InputDeviceType::RTLSDR && airspyRate) {
        Log::error("Stream1090") << "-s " << rate_mhz(rate)
                                 << " is not supported by RTL-SDR; use 2.4, 2.56 or 3.2 MHz.";
        return std::nullopt;
    }
    if (type == InputDeviceType::AIRSPY && !airspyRate) {
        Log::error("Stream1090") << "-s " << rate_mhz(rate) << " is not supported by Airspy; use 6 or 10 MHz.";
        return std::nullopt;
    }
    if (!has_input_rate(rate)) {
        Log::error("Stream1090") << "Unsupported input rate: " << rate_mhz(rate) << " MHz";
        print_rate_pairs();
        return std::nullopt;
    }
    return rate;
}

// Turns the command line into a DeviceConfig and applies the automatic choices
// for whatever the user left unset. Prints the reason and returns nullopt on
// failure.
inline std::optional<DeviceConfig> build_device_config(const CliArgs& args, InputDeviceType type,
                                                       SampleRate inputRate) {
    DeviceConfig cfg;

    const auto integer = [](const std::string& raw, auto& out, const char* what) {
        if (raw.empty())
            return true;
        if (!parse_number(raw, out)) {
            Log::error("Stream1090") << "Invalid " << what << ": " << raw;
            return false;
        }
        return true;
    };

    if (!args.serial.empty())
        cfg.serial = args.serial;
    if (!args.frequency.empty()) {
        unsigned long hz = 0;
        if (!integer(args.frequency, hz, "frequency") || hz == 0) {
            Log::error("Stream1090") << "Invalid frequency: " << args.frequency;
            return std::nullopt;
        }
        cfg.frequencyHz = static_cast<uint32_t>(hz);
    }
    if (!args.gain.empty() && !integer(args.gain, cfg.gainDb.emplace(), "gain"))
        return std::nullopt;
    cfg.agc = args.agc;
    cfg.biasTee = args.biasTee;
    if (!args.ppm.empty() && !integer(args.ppm, cfg.ppm.emplace(), "ppm"))
        return std::nullopt;
    if (!args.tunerBandwidth.empty()) {
        unsigned long hz = 0;
        if (!integer(args.tunerBandwidth, hz, "tuner bandwidth") || hz == 0) {
            Log::error("Stream1090") << "Invalid tuner bandwidth: " << args.tunerBandwidth;
            return std::nullopt;
        }
        cfg.tunerBandwidth = static_cast<uint32_t>(hz);
    }
    cfg.offsetTuning = args.offsetTuning;
    cfg.packing = args.packing;

    if (!args.linearityGain.empty() && !integer(args.linearityGain, cfg.linearityGain.emplace(), "linearity gain"))
        return std::nullopt;
    if (!args.sensitivityGain.empty() &&
        !integer(args.sensitivityGain, cfg.sensitivityGain.emplace(), "sensitivity gain"))
        return std::nullopt;
    if (!args.lnaGain.empty() && !integer(args.lnaGain, cfg.lnaGain.emplace(), "LNA gain"))
        return std::nullopt;
    if (!args.mixerGain.empty() && !integer(args.mixerGain, cfg.mixerGain.emplace(), "mixer gain"))
        return std::nullopt;
    if (!args.vgaGain.empty() && !integer(args.vgaGain, cfg.vgaGain.emplace(), "VGA gain"))
        return std::nullopt;

    // The auto-PPM on/off default comes from applyBackendDefaults (on for
    // RTL-SDR unless a fixed --ppm was given); an explicit --auto-ppm /
    // --no-auto-ppm wins over it.
    if (!integer(args.autoPpmWarmup, cfg.autoPpm.warmupSeconds, "auto ppm warmup"))
        return std::nullopt;
    if (!integer(args.autoPpmInterval, cfg.autoPpm.intervalSeconds, "auto ppm interval"))
        return std::nullopt;
    if (!integer(args.autoPpmSamples, cfg.autoPpm.samples, "auto ppm samples"))
        return std::nullopt;
    if (!integer(args.autoPpmMaxStep, cfg.autoPpm.maxStep, "auto ppm max step"))
        return std::nullopt;
    if (!integer(args.autoPpmDeadband, cfg.autoPpm.deadband, "auto ppm deadband"))
        return std::nullopt;
    if (!integer(args.autoPpmLimit, cfg.autoPpm.limit, "auto ppm limit"))
        return std::nullopt;

    cfg = applyBackendDefaults(cfg, type, inputRate);
    if (args.autoPpmSet)
        cfg.autoPpm.enabled = args.autoPpmEnabled;
    return cfg;
}

inline void print_backend_banner(InputDeviceType type) {
    if (type == InputDeviceType::AIRSPY) {
        Log::msg("Stream1090") << "Airspy backend";
        return;
    }
    if (type == InputDeviceType::RTLSDR) {
        if (GlobalOptions::RtlSdrBlogAdvanced)
            Log::msg("Stream1090") << "RTL-SDR backend: vendored rtl-sdr-blog fork";
        else
            Log::msg("Stream1090") << "RTL-SDR backend: external librtlsdr";
    }
}
