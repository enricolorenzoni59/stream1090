/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2025 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "Cli.hpp"
#include "DeviceSelection.hpp"
#include "MainInstance.hpp"
#include "PresetDispatcher.hpp"
#include "RateUtils.hpp"

void print_help() {
    std::cout << "Stream1090 build " << STREAM1090_VERSION << "\n";
    if (GlobalOptions::CustomInputMode) {
        std::cout << "(custom input mode)\n";
    }

    std::cout << "Native device support:" << std::endl;
    if (GlobalOptions::NativeAirspySupport) {
        std::cout << " Airspy";
    }

    if (GlobalOptions::NativeRtlSdrSupport) {
        if (GlobalOptions::RtlSdrBlogAdvanced) {
            std::cout << "  RTL-SDR Blog (advanced)";
        } else {
            std::cout << "  RTL-SDR";
        }
    }

    if (!GlobalOptions::NativeRtlSdrSupport && !GlobalOptions::NativeAirspySupport) {
        std::cout << " none";
    }

    std::cout << "\n\n";

    std::cout << "Usage:\n"
                 "  stream1090 [options]\n\n"
                 "Options:\n"
                 "  -s <rate>            Input sample rate in MHz (default: device dependent)\n"
                 "  -u <rate>            Upsample rate in MHz (default: highest for the input)\n"
                 "  -q                   Enables IQ FIR filter with built-in taps (on by default)\n"
                 "  --no-iq-filter       Disable the IQ FIR filter\n"
                 "  -f <taps file>       Taps to load that are used for the IQ FIR filter\n"
                 "  -v, --verbose        Verbose output\n"
                 "  --debug              Debug output (implies verbose)\n"
                 "  --net-bind-address <address>  TCP bind address (default: 127.0.0.1)\n"
                 "  --net-avr-port <port>          Enable AVR/raw TCP output\n"
                 "  --net-beast-port <port>        Enable Beast binary TCP output\n"
                 "  --no-stdout                    Disable legacy AVR stdout output\n"
                 "  --metrics [addr:port]          Serve Prometheus metrics on /metrics,\n"
                 "                                 plus /healthz and /readyz. Defaults to\n"
                 "                                 127.0.0.1:9109. There is no authentication,\n"
                 "                                 so a public interface is a deliberate choice\n"
                 "  -h, --help           Show this help message\n\n"
                 "Device options:\n"
                 "  --device <kind>      stdin, auto, airspy or rtlsdr. When omitted, a\n"
                 "                       piped stdin is used and an empty /dev/null otherwise\n"
                 "                       triggers auto detection (Airspy first, then RTL-SDR).\n"
                 "  --serial <id>        Select one unit; otherwise the first free one\n"
                 "  --freq <hz>          Center frequency (default: 1090000000)\n"
                 "  --gain <db>          RTL-SDR tuner gain (default: 49.6)\n"
                 "  --agc                Enable RTL-SDR AGC\n"
                 "  --bias-tee           Enable the 5V bias tee\n"
                 "  --ppm <int>          Frequency correction in PPM\n"
                 "  --tuner-bandwidth <hz>  RTL-SDR IF bandwidth, 0 lets librtlsdr derive it\n"
                 "                       (default: rate dependent)\n"
                 "  --offset-tuning      Enable RTL-SDR offset tuning\n"
                 "  --airspy-packing <bool>  Pack the 12-bit Airspy samples (default: true)\n"
                 "  --linearity-gain <n>     Airspy combined preset, 0..21\n"
                 "  --sensitivity-gain <n>   Airspy combined preset, 0..21\n"
                 "  --lna-gain/--mixer-gain/--vga-gain <n>  Per-stage manual gain\n"
                 "  --auto-ppm           RTL-SDR closed-loop crystal calibration (on by\n"
                 "                       default unless --ppm is given)\n"
                 "  --no-auto-ppm        Disable the calibration\n"
                 "  --auto-ppm-warmup <s> --auto-ppm-interval <s> --auto-ppm-samples <n>\n"
                 "  --auto-ppm-max-step <n> --auto-ppm-deadband <n> --auto-ppm-limit <n>\n\n";

    print_rate_pairs();

    std::cout << "Examples:\n"
                 "  ./build/stream1090 --device auto -s 2.56 -u 12 -q\n"
                 "  ./build/stream1090 --device rtlsdr --gain 40 --net-beast-port 30007\n"
                 "  rtl_sdr -f 1090000000 -s 2400000 - | ./build/stream1090 --device stdin -s 2.4 -q\n"
                 "  readsb --net --net-connector=127.0.0.1,30007,beast_in\n\n";
}

std::vector<float> load_taps_from_file(const std::string& filename) {
    std::vector<float> taps;
    std::ifstream file(filename);
    if (!file.is_open()) {
        return taps;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty())
            continue;

        if (line[0] == '#')
            continue;

        try {
            double v = std::stod(line);
            taps.push_back((float)v);
        } catch (...) {
            return std::vector<float>();
        }

        // too many taps
        if (taps.size() > 64) {
            return std::vector<float>();
        }
    }

    return taps;
}

// Returns nothing when no preset matches the requested configuration, and
// otherwise the outcome of the run. Dispatch is split per device backend so
// the heavy MainInstance<...> instantiations compile in their own TUs.
std::optional<bool> runInstanceFromPresets(const CompileTimeVars& c_vars, const RuntimeVars& r_vars) {
#if defined(STREAM1090_CUSTOM_INPUT) && STREAM1090_CUSTOM_INPUT
    return runPresetGroup(presets, c_vars, r_vars);
#else
    if (auto o = runRtlSdrPresets(c_vars, r_vars))
        return o;
    if (auto o = runAirspyPresets(c_vars, r_vars))
        return o;
    return std::nullopt;
#endif
}

// One full run: select the device, configure it and decode until it stops.
// A SIGHUP asks the supervisor in main() to call this again from scratch, so a
// newly plugged dongle can be picked up without restarting the process.
enum class RunOutcome { Clean, DeviceLost, Failed };

RunOutcome run_once(const CliArgs& args, bool quiet) {
    RuntimeVars r_vars;
    CompileTimeVars c_vars;
    r_vars.stdoutEnabled = args.stdoutEnabled;
    r_vars.tcpOutput.bindAddress = args.netBindAddress;
    r_vars.tcpOutput.avrPort = args.netAvrPort;
    r_vars.tcpOutput.beastPort = args.netBeastPort;
    r_vars.tcpOutput.enableAvr = args.netAvrPort != 0;
    r_vars.tcpOutput.enableBeast = args.netBeastPort != 0;
    r_vars.metricsBind = args.metricsBind;
    r_vars.verbose = args.verbose;

    // ------------------------
    // Device selection
    // ------------------------
    const auto choice = choose_device(args, quiet);
    if (!choice)
        return RunOutcome::Failed;

    r_vars.deviceType = choice->type;
    r_vars.deviceSerials = choice->serials;

    if (r_vars.deviceType == InputDeviceType::STREAM)
        Log::msg("Stream1090") << "Reading from Stdin";
    else
        print_backend_banner(r_vars.deviceType);

    // ------------------------
    // FIR taps loading
    // ------------------------
    if (!args.tapsFile.empty()) {
        r_vars.filterTaps = load_taps_from_file(args.tapsFile);
        if (r_vars.filterTaps.empty()) {
            Log::error("Stream1090") << "Error loading taps from " << args.tapsFile;
            return RunOutcome::Failed;
        }
        // Check the file's own numbers here, so a coefficient that cannot be
        // represented is reported as the user wrote it.
        try {
            FirDetail::requireTapsFitAccumulator(r_vars.filterTaps, args.tapsFile.c_str());
        } catch (const std::invalid_argument& error) {
            Log::error("Stream1090") << error.what();
            return RunOutcome::Failed;
        }
    }

    // ------------------------
    // Sample speed
    // ------------------------
    const bool nativeDevice = r_vars.deviceType == InputDeviceType::AIRSPY ||
                              r_vars.deviceType == InputDeviceType::RTLSDR;

    if (nativeDevice) {
        const auto rate = resolve_input_rate(args, r_vars.deviceType, r_vars.deviceSerials);
        if (!rate)
            return RunOutcome::Failed;
        c_vars.inputRate = *rate;

        if (r_vars.deviceType == InputDeviceType::RTLSDR && !GlobalOptions::RtlSdrBlogAdvanced &&
            c_vars.inputRate == Rate_3_2_Mhz) {
            Log::warn("Stream1090") << "the 3.2 Msps preset is tuned against the vendored\n"
                                       "rtl-sdr-blog fork (-DENABLE_RTLSDR_BLOG=ON). With the system\n"
                                       "librtlsdr the same settings land in a different tuner state\n"
                                       "and the preset loses ~35% frames.";
        }
    } else {
        // stdin needs an explicit rate, exactly as before.
        if (args.sampleRate.empty()) {
            print_help();
            return RunOutcome::Failed;
        }
        c_vars.inputRate = parse_sample_rate(args.sampleRate);
        if (!has_input_rate(c_vars.inputRate)) {
            Log::error("Stream1090") << "Unsupported input rate: " << rate_mhz(c_vars.inputRate) << " MHz";
            print_rate_pairs();
            return RunOutcome::Failed;
        }
    }

    // Output rate: explicit, or the highest upsample available for the input.
    if (!args.upsampleRate.empty()) {
        c_vars.outputRate = parse_sample_rate(args.upsampleRate);

        if (!is_valid_rate_pair(c_vars.inputRate, c_vars.outputRate)) {
            Log::error("Stream1090") << "Unsupported rate combination: " << rate_mhz(c_vars.inputRate) << " -> "
                                     << rate_mhz(c_vars.outputRate);
            print_rate_pairs();
            return RunOutcome::Failed;
        }
    } else {
        auto def = find_default_output_rate(c_vars.inputRate);
        if (!def) {
            Log::error("Stream1090") << "No valid output rate for input rate: " << rate_mhz(c_vars.inputRate);
            print_rate_pairs();
            return RunOutcome::Failed;
        }

        c_vars.outputRate = *def;

        Log::msg("Stream1090") << "Auto-selected output rate: " << rate_mhz(c_vars.outputRate) << " MHz";
    }

    // The device settings need the rate to resolve their automatic defaults.
    if (nativeDevice) {
        const auto config = build_device_config(args, r_vars.deviceType, c_vars.inputRate);
        if (!config)
            return RunOutcome::Failed;
        r_vars.deviceConfig = *config;
    }

    // ------------------------
    // Format and pipeline
    // ------------------------
    if (GlobalOptions::CustomInputMode) {
        c_vars.rawFormat = InputFormatType::IQ_FLOAT32;
        c_vars.pipelineOption = IQPipelineOptions::NONE;
    } else {
        // The backend decides the raw format; for stdin it is implied by the
        // requested rate, exactly as it always was.
        if (r_vars.deviceType == InputDeviceType::AIRSPY)
            c_vars.rawFormat = InputFormatType::IQ_UINT16_RAW_AIRSPY;
        else if (r_vars.deviceType == InputDeviceType::RTLSDR)
            c_vars.rawFormat = InputFormatType::IQ_UINT8_RTL_SDR;
        else
            c_vars.rawFormat = (c_vars.inputRate < Rate_6_0_Mhz) ? InputFormatType::IQ_UINT8_RTL_SDR
                                                                 : InputFormatType::IQ_UINT16_RAW_AIRSPY;

        c_vars.pipelineOption = IQPipelineOptions::NONE;
        if (!r_vars.filterTaps.empty()) {
            if (c_vars.rawFormat == InputFormatType::IQ_UINT8_RTL_SDR) {
                c_vars.pipelineOption = IQPipelineOptions::IQ_FIR_RTL_SDR_FILE;
            } else {
                c_vars.pipelineOption = IQPipelineOptions::IQ_FIR_FILE;
            }
        } else if (args.iq_filter) {
            if (c_vars.rawFormat == InputFormatType::IQ_UINT8_RTL_SDR) {
                c_vars.pipelineOption = IQPipelineOptions::IQ_FIR_RTL_SDR;
            } else {
                c_vars.pipelineOption = IQPipelineOptions::IQ_FIR;
            }
        }
    }

    // ------------------------
    // Let's go
    // ------------------------
    // Building the pipeline validates the tap sets, including the ones -f
    // supplied after the branch alignment has grown them. A filter that cannot
    // be represented is refused here rather than quietly reshaped, so the
    // failure names what is wrong instead of showing up as a thin output.
    std::optional<bool> outcome;
    try {
        outcome = runInstanceFromPresets(c_vars, r_vars);
    } catch (const std::invalid_argument& error) {
        Log::error("Stream1090") << error.what();
        return RunOutcome::Failed;
    }
    if (!outcome) {
        Log::error("Stream1090") << "Configuration is not supported: " << c_vars.inputRate << " -> "
                                 << c_vars.outputRate;
        return RunOutcome::Failed;
    }

    // A loss is reported by the watchdog through its own flag; anything else
    // that ended the run cleanly is a normal shutdown.
    if (ProcessSignals::deviceLostRequested())
        return RunOutcome::DeviceLost;
    return *outcome ? RunOutcome::Clean : RunOutcome::Failed;
}

int main(int argc, char** argv) {
    // Input and logging may run beside AVR output. Their default ties must not
    // flush std::cout concurrently from another thread.
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::cerr.tie(nullptr);

    CliArgs args;
    if (!parse_cli(argc, argv, args)) {
        std::cerr << "Usage: stream1090 [--device stdin|auto|airspy|rtlsdr] [-s <rate>] [-u <rate>] "
                     "[-f <taps file>] [-q] [--verbose] [--debug] [-h]\n";
        return 1;
    }
    if (args.helpRequested) {
        print_help();
        return 0;
    }

    if (args.netAvrPort != 0 && args.netAvrPort == args.netBeastPort) {
        std::cerr << "AVR and Beast TCP ports must be different.\n";
        return 1;
    }
    if (!args.stdoutEnabled && args.netAvrPort == 0 && args.netBeastPort == 0) {
        std::cerr << "--no-stdout requires --net-avr-port and/or --net-beast-port.\n";
        return 1;
    }

    if (args.verbose)
        Log::setLevel(Log::Level::INFO);
    if (args.debug)
        Log::setLevel(Log::Level::DEBUG);

    // Installed once, before any run, so a signal between two runs is never
    // lost and never falls back to the default action.
    ProcessSignals::install();

    // A lost device is retried a bounded number of times, one second apart, so
    // a re-enumerating USB device has a chance to come back. One attempt
    // budget is shared per loss event and reset when a fresh loss is seen.
    constexpr int kMaxRecoveryAttempts = 10;
    constexpr auto kRecoveryDelay = std::chrono::seconds(1);
    bool recovering = false;
    int recoveryAttempts = 0;

    for (;;) {
        if (ProcessSignals::reselectRequested()) {
            ProcessSignals::clearReselect();
            ProcessSignals::clearShutdown();
            ProcessSignals::clearDeviceLost();
            recovering = false;
            recoveryAttempts = 0;
            Log::msg("Stream1090") << "SIGHUP: selecting the device again.";
        }

        const RunOutcome runOutcome = run_once(args, recovering);

        // A SIGHUP during the run is handled at the top of the next iteration.
        if (ProcessSignals::reselectRequested())
            continue;

        if (runOutcome == RunOutcome::Clean)
            return 0;

        if (runOutcome == RunOutcome::DeviceLost) {
            // The device had been streaming, so this is a fresh loss: restart
            // the attempt budget and say so once.
            ProcessSignals::clearDeviceLost();
            ProcessSignals::clearShutdown();
            if (!recovering) {
                Log::warn("Stream1090") << "Device lost; trying to recover (up to " << kMaxRecoveryAttempts
                                        << " attempts, " << kRecoveryDelay.count() << " s apart).";
            }
            recovering = true;
            recoveryAttempts = 0;
        } else if (!recovering) {
            // A setup failure with no prior loss is not a recovery situation.
            return 1;
        }

        if (recoveryAttempts >= kMaxRecoveryAttempts) {
            Metrics::registry().deviceRecoveryFailed.inc();
            Log::error("Stream1090") << "Device did not come back after " << kMaxRecoveryAttempts
                                     << " attempts; exiting.";
            return 1;
        }

        ++recoveryAttempts;
        Metrics::registry().deviceRecoveryAttempts.inc();
        Log::warn("Stream1090") << "Recovery attempt " << recoveryAttempts << "/" << kMaxRecoveryAttempts << ".";
        std::this_thread::sleep_for(kRecoveryDelay);

        // Ctrl-C during recovery still means stop.
        if (ProcessSignals::shutdownRequested() && !ProcessSignals::reselectRequested()) {
            Log::warn("Stream1090") << "Shutdown requested during recovery; exiting.";
            return 1;
        }
    }
}
