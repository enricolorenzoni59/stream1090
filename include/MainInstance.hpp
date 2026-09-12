/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once
#include "Global.hpp"
#include "Logger.hpp"
#include "Sampler.hpp"
#include "RingBuffer.hpp"
#include "Presets.hpp"
#include "SampleStream.hpp"
#include "InputStreamReader.hpp"
#include "InputBufferReader.hpp"
#include "IQPipeline.hpp"
#include "LowPassFilter.hpp"
#include "devices/IniConfig.hpp"
#include "devices/DeviceFactory.hpp"
#include "TcpOutputServer.hpp"
#include "Metrics.hpp"
#include "MetricsServer.hpp"
#include <chrono>
#include <deque>
#include <cstdlib>
#include <algorithm>
#include <optional>
#include <sstream>
#include <unistd.h>

template <typename Sampler> void printSamplerConfig() {
    std::cerr << "[Stream1090] build " << STREAM1090_VERSION << std::endl;
    std::cerr << "[Stream1090] Input sampling speed: " << (double)Sampler::InputSampleRate / 1000000.0 << " MHz"
              << std::endl;
    std::cerr << "[Stream1090] Output sampling speed: " << Sampler::OutputSampleRate / 1000000 << " MHz" << std::endl;
    std::cerr << "[Stream1090] Input to output ratio: " << Sampler::RatioInput << ":" << Sampler::RatioOutput
              << std::endl;
    std::cerr << "[Stream1090] Number of streams: " << Sampler::NumStreams << std::endl;
    std::cerr << "[Stream1090] Size of input buffer: " << Sampler::InputBufferSize << " samples " << std::endl;
    std::cerr << "[Stream1090] Size of sample buffer: " << Sampler::SampleBufferSize << " samples " << std::endl;
}

struct CompileTimeVars {
    InputFormatType rawFormat = InputFormatType::IQ_UINT8_RTL_SDR;
    SampleRate inputRate = Rate_2_4_Mhz;
    SampleRate outputRate = Rate_8_0_Mhz;
    IQPipelineOptions pipelineOption = IQPipelineOptions::NONE;
};

struct RuntimeVars {
    InputDeviceType deviceType = InputDeviceType::STREAM;
    IniConfig deviceConfig;
    IniConfig::Section deviceConfigSection;
    std::vector<float> filterTaps;
    bool verbose = true;
    bool stdoutEnabled = true;
    TcpOutputConfig tcpOutput;
    // Empty leaves the Prometheus scrape endpoint off. See --metrics.
    std::string metricsBind;
};

// this class serves to hold all compile and runtime information
template <typename preset> class MainInstance {
  public:
    MainInstance(const RuntimeVars& runtimeVars) : m_runtimeVars(runtimeVars) {
        printSamplerConfig<SamplerType>();
    }

    // we first unpack the preset
    using RawFormatType = typename preset::RawFormatType;
    using RawType = typename preset::RawType;
    using SamplerType = typename preset::SamplerType;

    static constexpr SampleRate inputRate = SamplerType::InputSampleRate;
    static constexpr SampleRate outputRate = SamplerType::OutputSampleRate;
    static constexpr IQPipelineOptions pipelineOption = preset::pipelineOption;

    // with all the compile time information available we continue now with what we need
    using DevicePtr = std::unique_ptr<InputDeviceBase<RawType>>;
    using RingBuffer = RingBufferAsync<RawType, SamplerType::InputBufferSize * 2>;
    using Writer = typename RingBuffer::Writer;

    bool reloadDeviceConfig() {
        // Re-read the INI file from disk
        if (!m_runtimeVars.deviceConfig.reload()) {
            Log::error("Stream1090", "Failed to reload INI file.");
            return false;
        }

        auto& cfg = m_runtimeVars.deviceConfig.get();

        // Extract the correct section
        if (m_runtimeVars.deviceType == InputDeviceType::AIRSPY) {
            if (!cfg.count("airspy")) {
                Log::error("Stream1090", "Reloaded INI missing [airspy] section.");
                return false;
            }
            m_runtimeVars.deviceConfigSection = cfg.at("airspy");
        }

        else if (m_runtimeVars.deviceType == InputDeviceType::RTLSDR) {
            if (!cfg.count("rtlsdr")) {
                Log::error("Stream1090", "Reloaded INI missing [rtlsdr] section.");
                return false;
            }
            m_runtimeVars.deviceConfigSection = cfg.at("rtlsdr");
        }

        return true;
    }

    static const char* deviceName(InputDeviceType type) {
        switch (type) {
        case InputDeviceType::AIRSPY:
            return "airspy";
        case InputDeviceType::RTLSDR:
            return "rtlsdr";
        default:
            return "stream";
        }
    }

    /// Mirrors the configuration that was actually applied to the device, so a
    /// change of gain or ppm can be seen next to what it did to the counters.
    void publishDeviceSettings() {
        if constexpr (!Metrics::Enabled)
            return;
        const auto& cfg = m_runtimeVars.deviceConfigSection;
        auto& reg = Metrics::registry();
        const auto number = [&cfg](const char* key, double fallback) {
            const auto it = cfg.find(key);
            if (it == cfg.end())
                return fallback;
            try {
                return std::stod(it->second);
            } catch (const std::exception&) {
                return fallback;
            }
        };
        reg.settingPpm.set(number("ppm", 0.0));
        reg.settingFrequencyHz.set(number("frequency", 1090000000.0));
        const auto agc = cfg.find("agc");
        reg.settingAgc.set(agc != cfg.end() && (agc->second == "1" || agc->second == "true") ? 1.0 : 0.0);

        if (m_device) {
            const auto gain = m_device->gainState();
            reg.setGainState(gain.hasStages, gain.db, gain.mode, gain.autoGain, double(gain.overall),
                             double(gain.lna), double(gain.mixer), double(gain.vga));
        }
    }

    bool setup_device() {
        const auto& cfg = m_runtimeVars.deviceConfigSection;

        // tell the device to read all properties required to open it
        Log::info("Stream1090", "Reading initial properties from the ini file");
        m_device->applyConfigPreOpen(cfg);

        // let us try to open the device
        Log::info("Stream1090", "Trying to open the device.");
        if (!m_device->open()) {
            Log::error("Stream1090", "Opening device failed.");
            // this is not good at all
            return false;
        };

        // device is ready, apply all the other properties
        Log::info("Stream1090", "Device is open. Reading the ini file.");
        m_device->applyConfigPostOpen(cfg);

        // we do not care if any of the properties did not work
        return true;
    }

    auto constructMessageHandler(SampleStream<SamplerType>& sampleStream, TcpOutputServer* tcpServer) {
        if constexpr (GlobalOptions::RSSIEnabled) {
            return RssiStdOutMessageHandler<SamplerType, SampleStream<SamplerType>>(
                sampleStream, m_runtimeVars.stdoutEnabled, tcpServer);
        } else {
            return StdOutMessageHandler<SamplerType>(m_runtimeVars.stdoutEnabled, tcpServer);
        }
    }

    bool run_async_device(auto& iqPipeline) {
        TcpOutputServer tcpServer(m_runtimeVars.tcpOutput);
        TcpOutputServer* tcp = nullptr;
        if (m_runtimeVars.tcpOutput.enableAvr || m_runtimeVars.tcpOutput.enableBeast) {
            std::string error;
            if (!tcpServer.start(error)) {
                Log::error("TCP", error);
                return false;
            }
            tcp = &tcpServer;
            if (m_runtimeVars.tcpOutput.enableAvr)
                Log::info("TCP") << "AVR server listening on " << m_runtimeVars.tcpOutput.bindAddress << ':'
                                 << tcpServer.avrPort();
            if (m_runtimeVars.tcpOutput.enableBeast)
                Log::info("TCP") << "Beast server listening on " << m_runtimeVars.tcpOutput.bindAddress << ':'
                                 << tcpServer.beastPort();
        }
        RingBuffer ringBuffer;
        Writer writer(ringBuffer);

        m_device = DeviceFactory<RawType>::create(m_runtimeVars.deviceType, inputRate, writer);
        if (!m_device) {
            Log::error("Stream1090", "Device instantiation failed.");
            return false;
        }
        Log::info("Stream1090", "Device created.");

        if (!setup_device()) {
            Log::error("Stream1090", "Device configuration failed.");
            return false;
        }
        Log::info("Stream1090", "Device successfully configured.");

        if (!m_device->start()) {
            Log::error("Stream1090", "Device refuses to start. Aborting.");
            return false;
        }
        Log::info("Stream1090", "Device is running. ");

        Log::info("Stream1090", "Installing sig handlers.");
        ProcessSignals::install();

        // If we made it until here, we assume that this device is ready and alive.
        // We mark it here as such, because especially the rtlsdr driver needs some
        // time to startup which sometimes may exceed the timeout of the watchdog.
        // In other words, the driver is still initializing, but takes so long that
        // the watchdog thinks it is dead and tries to kill it.
        m_device->markAsAlive();
        Metrics::registry().deviceUp.set(1.0);
        publishDeviceSettings();
        Log::info("Stream1090", "Devices has been marked as alive.");

        // flag that indicates if the shutdown was intended
        // or the watchhdog killed the device
        bool intendedShutdown = true;

        // -------------------------------
        // WATCHDOG THREAD
        // -------------------------------
        std::thread watchdog([this, &intendedShutdown, tcp] {
            using namespace std::chrono_literals;
            Log::info("Watchdog", "Started.");

            // -------------------------------
            // SAMPLE-DROP MONITOR STATE
            //
            // The detection runs on the device callback thread (see
            // InputDeviceBase::writeDataToBuffer): delivered IQ pairs
            // against the wall clock, measured at arrival. This side only
            // warns and decides whether the run may continue. The exit
            // policy defaults to "more than 1 drop per 60 s", overridable
            // with STREAM1090_MAX_DROPS_PER_MIN (0 disables the exit,
            // warnings stay on).
            // -------------------------------
            const uint64_t iqPairsPerSec = (uint64_t)inputRate;
            int maxDropsPerMin = 1;
            if (const char* env = std::getenv("STREAM1090_MAX_DROPS_PER_MIN")) {
                try {
                    maxDropsPerMin = std::max(0, std::stoi(env));
                } catch (...) {
                }
            }
            Log::info("Watchdog") << "Sample-drop monitor active: exit after more than " << maxDropsPerMin
                                  << " drop(s) in 60 s"
                                  << (maxDropsPerMin == 1 ? " (STREAM1090_MAX_DROPS_PER_MIN to override)" : "") << ".";
            uint64_t seenDropEvents = 0;
            std::deque<std::chrono::steady_clock::time_point> recentDrops;

            while (!ProcessSignals::shutdownRequested()) {
                if (m_device) {
                    const auto lastSign = m_device->lastSignOfLife();
                    Metrics::registry().deviceLastSampleAge.set(double(lastSign.count()) / 1000.0);
                    if (lastSign > 1000ms) {
                        // 1) Device health check. Is the device still alive?
                        Metrics::registry().watchdogLost.inc();
                        Metrics::registry().deviceUp.set(0.0);
                        Log::error("Watchdog") << "No samples for more than 1000ms. Device lost? Initiating shutdown.";
                        // Only wake the pipeline here, and leave the device to the
                        // shutdown path below, which closes it in every case.
                        // Closing from this thread as well means two threads run
                        // close() concurrently: both reach rtlsdr_close/airspy_close
                        // on the same handle and both join the same reader thread.
                        m_device->shutdownWriter();
                        ProcessSignals::handle_sigint(0);
                        // mark that the shutdown was not intended
                        intendedShutdown = false;
                        break;
                    } else if (lastSign > 100ms) {
                        // 2) Device health check. Issue a warning if the device falls behind.
                        Metrics::registry().watchdogLate.inc();
                        Log::warn("Watchdog") << "No samples for " << lastSign << ". The device is falling behind.";
                    }
                }

                // 3) Reload request (SIGHUP)
                if (ProcessSignals::reloadRequested()) {
                    ProcessSignals::clearReload();
                    Log::info("Stream1090", "Re-reading config file.");

                    if (reloadDeviceConfig()) {
                        Log::info("Stream1090", "Applying new configuration.");
                        m_device->applyConfigPostOpen(m_runtimeVars.deviceConfigSection);
                        auto& reg = Metrics::registry();
                        reg.configReloadsOk.inc();
                        reg.configGeneration.set(reg.configGeneration.get() + 1.0);
                        publishDeviceSettings();
                    } else {
                        Metrics::registry().configReloadsFailed.inc();
                        Log::warn("Stream1090", "Reload failed. Keeping old settings.");
                    }
                }

                // 4) Sample-drop policy. The detection runs on the device
                // callback thread; this side only warns and decides whether
                // the run may continue.
                if (m_device) {
                    const auto now = std::chrono::steady_clock::now();
                    const uint64_t events = m_device->dropEventCount();
                    while (seenDropEvents < events) {
                        seenDropEvents++;
                        recentDrops.push_back(now);
                        while (!recentDrops.empty() && now - recentDrops.front() > 60s)
                            recentDrops.pop_front();
                        Metrics::registry().sampleDropEvents.inc();
                        Metrics::registry().sampleDropDeficit.set(double(m_device->currentDropDeficit()));
                        Metrics::registry().sampleDropWorst.set(double(m_device->maxDropDeficit()));
                        Log::warn("Watchdog") << "Sample drop: ~" << m_device->lastEventGrowth() << " IQ pairs (~"
                                              << (double)m_device->lastEventGrowth() / (double)iqPairsPerSec * 1000.0
                                              << " ms of stream) lost; cumulative deficit ~"
                                              << m_device->currentDropDeficit() << " IQ pairs.";
                        if (maxDropsPerMin > 0 && recentDrops.size() > (size_t)maxDropsPerMin) {
                            Log::error("Watchdog")
                                << recentDrops.size() << " sample drops in the last 60 s (threshold: more than "
                                << maxDropsPerMin << "). The host cannot sustain this sample rate. "
                                << "Initiating shutdown.";
                            intendedShutdown = false;
                            m_device->shutdownWriter();
                            ProcessSignals::handle_sigint(0);
                            break;
                        }
                    }
                }

                if (tcp)
                    Metrics::registry().tcpClients.set(double(tcp->clientCount()));

                std::this_thread::sleep_for(200ms);
            }
            if (seenDropEvents > 0)
                Log::warn("Watchdog") << "Sample drops during this run: " << seenDropEvents << " event(s), worst gap ~"
                                      << m_device->maxDropDeficit() << " IQ pairs.";
            Log::info("Watchdog", "Watchdog is done.");
        });

        // -------------------------------
        // DSP PIPELINE (blocking)
        // -------------------------------
        auto start_wct = std::chrono::steady_clock::now();

        if (m_device->isRunning()) {
            Log::info("Stream1090", "Device is running, starting stream.");
            InputBufferReader<RawFormatType, SamplerType::InputBufferSize * 2, 8, decltype(iqPipeline)> inputReader(
                iqPipeline, ringBuffer);

            SampleStream<SamplerType> sampleStream;
            auto messageHandler = constructMessageHandler(sampleStream, tcp);

            sampleStream.read(inputReader, messageHandler);
        }

        // -------------------------------
        // SHUTDOWN
        // -------------------------------
        Log::info("Stream1090", "Shutting down device.");
        Metrics::registry().deviceUp.set(0.0);
        m_device->close();
        Log::info("Stream1090", "Device closed down.");

        auto end_wct = std::chrono::steady_clock::now();
        auto dur_wct_secs = std::chrono::duration_cast<std::chrono::milliseconds>(end_wct - start_wct).count();
        if (watchdog.joinable()) {
            Log::info("Stream1090", "Watchdog joining.");
            watchdog.join();
            Log::info("Stream1090", "Watchdog joined.");
        }
        Log::info("Stream1090", "Shutdown completed.");
        tcpServer.stop();
        if (tcp) {
            auto& reg = Metrics::registry();
            reg.tcpClients.set(0.0);
            reg.tcpFramesDropped.inc(tcpServer.droppedFrames());
            reg.tcpSlowDisconnects.inc(tcpServer.slowClientDisconnects());
            Log::info("TCP") << "Stopped: " << tcpServer.droppedFrames() << " frame(s) dropped, "
                             << tcpServer.slowClientDisconnects() << " slow client(s) disconnected.";
        }
        Log::msg("Stream1090") << "Finished. (" << dur_wct_secs / 1000.0 << "s)";
        // return if this shutdown was intended or not (lost device)
        return intendedShutdown;
    }

    bool run_sync_stdin(auto& iqPipeline) {
        TcpOutputServer tcpServer(m_runtimeVars.tcpOutput);
        TcpOutputServer* tcp = nullptr;
        if (m_runtimeVars.tcpOutput.enableAvr || m_runtimeVars.tcpOutput.enableBeast) {
            std::string error;
            if (!tcpServer.start(error)) {
                Log::error("TCP", error);
                return false;
            }
            tcp = &tcpServer;
        }
        Log::info("Stream1090", "Reading from stdin");
        Metrics::registry().deviceUp.set(1.0);
        auto start_wct = std::chrono::steady_clock::now();

        InputStdStreamReader<RawFormatType, SamplerType::InputBufferSize, decltype(iqPipeline)> inputReader(
            iqPipeline, STDIN_FILENO);

        SampleStream<SamplerType> sampleStream;
        auto messageHandler = constructMessageHandler(sampleStream, tcp);
        sampleStream.read(inputReader, messageHandler);
        tcpServer.stop();
        Metrics::registry().deviceUp.set(0.0);
        if (tcp) {
            auto& reg = Metrics::registry();
            reg.tcpClients.set(0.0);
            reg.tcpFramesDropped.inc(tcpServer.droppedFrames());
            reg.tcpSlowDisconnects.inc(tcpServer.slowClientDisconnects());
        }

        auto end_wct = std::chrono::steady_clock::now();
        auto dur_wct_secs = std::chrono::duration_cast<std::chrono::milliseconds>(end_wct - start_wct).count();
        Log::msg("Stream1090") << "Finished. (" << dur_wct_secs / 1000.0 << "s)";
        return true;
    }

    bool run() {
        // setup pipeline
        auto iqPipeline = IQPipelineSelector<inputRate, outputRate, pipelineOption>().make(m_runtimeVars.filterTaps);
        const std::string pipelineName = iqPipeline.toString();
        Log::info("", pipelineName);

        auto& reg = Metrics::registry();
        reg.setBuildInfo(STREAM1090_VERSION, pipelineName.empty() ? std::string("none") : pipelineName,
                         uint32_t(inputRate), uint32_t(outputRate), uint32_t(SamplerType::NumStreams));
        reg.setDeviceName(deviceName(m_runtimeVars.deviceType));

        MetricsServer metricsServer;
        if (!m_runtimeVars.metricsBind.empty()) {
            if constexpr (Metrics::Enabled) {
                if (!metricsServer.start(m_runtimeVars.metricsBind))
                    Log::warn("Metrics", "Continuing without the metrics endpoint.");
            } else {
                Log::warn("Metrics", "This build has metrics compiled out, ignoring --metrics.");
            }
        }

        // for sync read from std in we take a short cut
        bool outcome;
        if (m_runtimeVars.deviceType == InputDeviceType::STREAM) {
            Log::info("Stream1090", "Sync Stdin Mode");
            outcome = run_sync_stdin(iqPipeline);
        } else {
            Log::info("Stream1090", "Async Device Mode");
            outcome = run_async_device(iqPipeline);
        }

        metricsServer.stop();
        return outcome;
    }

  private:
    DevicePtr m_device = nullptr;
    RuntimeVars m_runtimeVars;
};
