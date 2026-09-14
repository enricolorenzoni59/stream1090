/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include <rtl-sdr.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "devices/InputDeviceBase.hpp"
#include "devices/DeviceConfig.hpp"

class RtlSdrDevice : public InputDeviceBase<uint8_t> {
  public:
    RtlSdrDevice(SampleRate sampleRate, IAsyncWriter<uint8_t>& bufferWriter)
        : InputDeviceBase<uint8_t>(sampleRate, bufferWriter) {}

    bool open() override;
    bool start() override;
    void stop() override;
    void close() override;
    void periodicMaintenance() override;
    void markDeviceLost() override;

    // Runtime setters (shadow-aware)
    bool setFrequency(uint32_t hz);
    bool setGain(float gainDb);
    bool setAgc(bool enabled);
    bool setBiasTee(bool enabled);
    bool setPpm(int ppm);
    bool setOffsetTuning(bool enabled);
    bool setTunerBandwidth(uint32_t bw);
    bool setLnaGain(int value);
    bool setMixerGain(int value);
    bool setVgaGain(int value);

    // Called before opening the device to parse the serial
    void applyConfigPreOpen(const DeviceConfig& cfg) override;

    // Reload hook
    void applyConfigPostOpen(const DeviceConfig& cfg) override;
    // Applied gain as the device shadow state sees it.
    GainState gainState() const override;
    int frequencyCorrectionPpm() const override { return m_state.ppm; }

  private:
    static void callback(unsigned char* buf, uint32_t len, void* ctx);
    void observeSamples(uint32_t len);
    void configureAutoPpm(const AutoPpmConfig& cfg);
    void resetAutoPpmMeasurement();
    bool open_with_serial(uint64_t serial = 0);
    bool open_with_serial(const std::string& serial);

    int nearestGain(int requested);

    struct ShadowState {
        uint32_t frequency = 1090000000;
        float gain_db = 0.0f;
        bool agc = false;
        bool bias_tee = false;
        int ppm = 0;
        bool offset_tuning = false;
        int lna_gain = 5;
        int mixer_gain = 5;
        int vga_gain = 5;
        uint32_t tuner_bandwidth = 0;
    };

    ShadowState m_state;
    bool m_initialConfigApplied = false;
    bool m_stateReported = false;
    // Set once stop() has begun, so the reader thread can tell a clean
    // shutdown from an unexpected read_async failure.
    std::atomic<bool> m_stopping{false};
    // Set by the watchdog when the device stops delivering samples.
    bool m_deviceLost = false;

    rtlsdr_dev_t* m_dev = nullptr;
    std::thread m_thread;
    uint64_t m_actualSerial = 0;
    std::string m_serialString = "";
    uint32_t m_openFrequency = 1090000000;
    // The tuner and ADC share the RTL-SDR crystal. Measuring the ADC sample
    // rate against steady_clock therefore measures the residual oscillator
    // error without relying on the (rather loose) carrier accuracy of remote
    // Mode-S transponders.
    struct AutoPpmState {
        bool enabled = false;
        unsigned intervalSeconds = 20;
        unsigned warmupSeconds = 30;
        unsigned samples = 5;
        int maxStep = 20;
        int deadband = 2;
        int limit = 200;

        uint64_t totalPairs = 0;
        uint64_t baselinePairs = 0;
        uint64_t baselineDropEvents = 0;
        std::chrono::steady_clock::time_point firstSample{};
        std::chrono::steady_clock::time_point latestSample{};
        std::chrono::steady_clock::time_point baselineTime{};
        bool haveSample = false;
        bool haveBaseline = false;
        std::vector<double> measurements;
    };
    AutoPpmState m_autoPpm;
    std::mutex m_autoPpmMutex;
};
