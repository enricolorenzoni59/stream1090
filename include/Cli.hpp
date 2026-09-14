/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>

// Everything the command line can set. Numeric device fields stay as strings
// here and are validated when the DeviceConfig is built, so a typo is reported
// against the exact flag the user typed.
struct CliArgs {
    std::string sampleRate = "";
    std::string upsampleRate = "";
    std::string tapsFile = "";
    bool iq_filter = true;
    bool verbose = false;
    bool debug = false;
    std::string netBindAddress = "127.0.0.1";
    uint16_t netAvrPort = 0;
    uint16_t netBeastPort = 0;
    bool stdoutEnabled = true;
    std::string metricsBind = "";
    bool helpRequested = false;

    // Device selection and configuration.
    std::string device = "";
    std::string serial = "";
    std::string frequency = "";
    std::string gain = "";
    bool agc = false;
    bool biasTee = false;
    std::string ppm = "";
    std::string tunerBandwidth = "";
    bool offsetTuning = false;
    bool packing = true;
    std::string linearityGain = "";
    std::string sensitivityGain = "";
    std::string lnaGain = "";
    std::string mixerGain = "";
    std::string vgaGain = "";
    bool autoPpmEnabled = false;
    // Whether --auto-ppm or --no-auto-ppm was given. Distinguishes "leave the
    // backend default" from "explicitly off".
    bool autoPpmSet = false;
    std::string autoPpmWarmup = "";
    std::string autoPpmInterval = "";
    std::string autoPpmSamples = "";
    std::string autoPpmMaxStep = "";
    std::string autoPpmDeadband = "";
    std::string autoPpmLimit = "";
};

inline bool parse_tcp_port(const std::string& value, uint16_t& port) {
    try {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed, 10);
        if (consumed != value.size() || parsed == 0 || parsed > 65535)
            return false;
        port = static_cast<uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool parse_bool(const std::string& value, bool& out) {
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        out = false;
        return true;
    }
    return false;
}

inline bool unknown_argument(const std::string& arg) {
    std::cerr << "Unknown or incomplete argument: " << arg << "\n";
    return false;
}

// Accepts decimal and 0x-prefixed integers, and floats where T is floating
// point. The whole string must be consumed.
template <typename T> bool parse_number(const std::string& value, T& out) {
    try {
        std::size_t pos = 0;
        if constexpr (std::is_integral_v<T>) {
            const long long parsed = std::stoll(value, &pos, 0);
            if (pos != value.size())
                return false;
            out = static_cast<T>(parsed);
        } else {
            const float parsed = std::stof(value, &pos);
            if (pos != value.size())
                return false;
            out = static_cast<T>(parsed);
        }
        return true;
    } catch (...) {
        return false;
    }
}

// Parses argv into `out`. --help sets helpRequested instead of exiting, so the
// caller owns the output.
inline bool parse_cli(int argc, char** argv, CliArgs& out) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        // Support both "--flag value" and "--flag=value". Only long options
        // take the inline form; the short ones stay as they were.
        std::string inlineValue;
        bool hasInline = false;
        if (arg.rfind("--", 0) == 0) {
            const auto eq = arg.find('=');
            if (eq != std::string::npos) {
                inlineValue = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                hasInline = true;
            }
        }

        const auto take = [&](std::string& target) {
            if (hasInline) {
                target = inlineValue;
                return true;
            }
            if (i + 1 >= argc)
                return false;
            target = argv[++i];
            return true;
        };
        // A bare flag means "on"; an explicit value is parsed. "--flag=false"
        // and "--flag false" are both accepted.
        const auto takeBoolean = [&](bool& target) {
            if (hasInline)
                return parse_bool(inlineValue, target);
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                std::string value = argv[++i];
                return parse_bool(value, target);
            }
            target = true;
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            out.helpRequested = true;
            return true;
        }

        if (arg == "-s" && i + 1 < argc) {
            out.sampleRate = argv[++i];
            continue;
        }

        if (arg == "-u" && i + 1 < argc) {
            out.upsampleRate = argv[++i];
            continue;
        }

        if (arg == "-f" && i + 1 < argc) {
            out.tapsFile = argv[++i];
            continue;
        }

        if (arg == "-q") {
            out.iq_filter = true;
            continue;
        }

        if (arg == "--no-iq-filter") {
            out.iq_filter = false;
            continue;
        }

        if ((arg == "-v") || (arg == "--verbose")) {
            out.verbose = true;
            continue;
        }

        if (arg == "--debug") {
            out.debug = true;
            continue;
        }

        if (arg == "--net-bind-address") {
            if (!take(out.netBindAddress))
                return unknown_argument(arg);
            continue;
        }

        if (arg == "--net-avr-port") {
            std::string value;
            if (!take(value) || !parse_tcp_port(value, out.netAvrPort)) {
                std::cerr << "Invalid AVR TCP port: " << value << "\n";
                return false;
            }
            continue;
        }

        if (arg == "--net-beast-port") {
            std::string value;
            if (!take(value) || !parse_tcp_port(value, out.netBeastPort)) {
                std::cerr << "Invalid Beast TCP port: " << value << "\n";
                return false;
            }
            continue;
        }

        if (arg == "--no-stdout") {
            out.stdoutEnabled = false;
            continue;
        }

        if (arg == "--metrics") {
            // The address is optional: "--metrics" alone takes the default.
            if (hasInline) {
                out.metricsBind = inlineValue;
            } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                out.metricsBind = argv[++i];
            } else {
                out.metricsBind = "127.0.0.1:9109";
            }
            continue;
        }

        // ---- device selection and configuration ----
        if (arg == "--device") {
            if (!take(out.device))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--serial") {
            if (!take(out.serial))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--freq") {
            if (!take(out.frequency))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--gain") {
            if (!take(out.gain))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--agc") {
            if (!takeBoolean(out.agc))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--bias-tee") {
            if (!takeBoolean(out.biasTee))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--ppm") {
            if (!take(out.ppm))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--tuner-bandwidth") {
            if (!take(out.tunerBandwidth))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--offset-tuning") {
            if (!takeBoolean(out.offsetTuning))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--airspy-packing") {
            if (!takeBoolean(out.packing))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--linearity-gain") {
            if (!take(out.linearityGain))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--sensitivity-gain") {
            if (!take(out.sensitivityGain))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--lna-gain") {
            if (!take(out.lnaGain))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--mixer-gain") {
            if (!take(out.mixerGain))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--vga-gain") {
            if (!take(out.vgaGain))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--auto-ppm") {
            if (!takeBoolean(out.autoPpmEnabled))
                return unknown_argument(arg);
            out.autoPpmSet = true;
            continue;
        }
        if (arg == "--no-auto-ppm") {
            out.autoPpmEnabled = false;
            out.autoPpmSet = true;
            continue;
        }
        if (arg == "--auto-ppm-warmup") {
            if (!take(out.autoPpmWarmup))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--auto-ppm-interval") {
            if (!take(out.autoPpmInterval))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--auto-ppm-samples") {
            if (!take(out.autoPpmSamples))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--auto-ppm-max-step") {
            if (!take(out.autoPpmMaxStep))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--auto-ppm-deadband") {
            if (!take(out.autoPpmDeadband))
                return unknown_argument(arg);
            continue;
        }
        if (arg == "--auto-ppm-limit") {
            if (!take(out.autoPpmLimit))
                return unknown_argument(arg);
            continue;
        }

        return unknown_argument(arg);
    }

    return true;
}
