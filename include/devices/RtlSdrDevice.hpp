/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include <rtl-sdr.h>
#include <thread>
#include <mutex>
#include "devices/InputDeviceBase.hpp"
#include "IniConfig.hpp"

class RtlSdrDevice : public InputDeviceBase<uint8_t> {
public:
    RtlSdrDevice(SampleRate sampleRate, IAsyncWriter<uint8_t>& bufferWriter)
        : InputDeviceBase<uint8_t>(sampleRate, bufferWriter) {}

    bool open() override;
    bool start() override;
    void stop() override;
    void close() override;

    // Called by the device callback for the adaptive controller.
    void accumulateGainStats(const unsigned char* buf, uint32_t len);

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
    void applyConfigPreOpen(const IniConfig::Section& cfg) override;

    // Reload hook
    void applyConfigPostOpen(const IniConfig::Section& cfg) override;
    
private:
    bool open_with_serial(uint64_t serial = 0);
    bool open_with_serial(const std::string& serial);
    bool applySetting(const std::string& key, const std::string& value);

    int nearestGain(int requested);
    bool vgaSupported() const;
    bool applyVgaGain(int value);

    struct ShadowState {
        uint32_t frequency = 1090000000;
        float gain_db = 0.0f;
        bool gain_known = false;
        bool agc = false;
        bool bias_tee = false;
        int ppm = 0;
        bool offset_tuning = false;
        int lna_gain = 5;
        bool lna_known = false;
        int mixer_gain = 5;
        bool mixer_known = false;
        int vga_gain = 5;
        bool vga_known = false;
        uint32_t tuner_bandwidth = 0;
    };

    ShadowState m_state;
    mutable std::mutex m_controlMutex;
    bool m_agcApplied = false;
    bool m_gainControlReady = false;
    bool m_stateReported = false;
    bool m_initialConfigApplied = false;

    // -------------------------------
    // Adaptive gain (always active on RTL-SDR in this test branch)
    //
    // With a linear front end, amplitude ratios are gain-invariant: the
    // only absolute references are the 8-bit ADC rails. The control
    // variable is the median noise radius p50, converted to a floor
    // sigma in LSB (p50/0.6745): immune to strong burst duty cycles
    // below 50%, and it tracks the environment one to one - attenuate
    // the input by X dB and the loop answers with X dB more tuner gain,
    // within one step. The setpoint band [2.5, 5] LSB comes from the
    // measured frame-rate optima with and without LNA (both sit at
    // sigma_floor = 3..5):
    //   p50 > 30 for two consecutive windows: step down 3 (hard overload)
    //   sigma_floor > 5:  step down (headroom to the rails wasted)
    //   sigma_floor < 2.5: step up (the fixed digital noise floor eats
    //                       the signal; this is what happens at low
    //                       tuner gain without an LNA)
    //   RMS > 16:  step down (headroom safety, rare)
    // Burst saturation from strong nearby transmitters is deliberately
    // NOT chased: strong signals decode clipped, and chasing them would
    // drop the gain exactly when the bursts leave.
    // -------------------------------
    void startAdaptiveGain();
    void stopAdaptiveGain();
    void adaptiveGainLoop();

    std::thread m_adaptiveThread;
    std::atomic<bool> m_adaptiveRun{false};

    mutable std::mutex m_configMutex;
    std::mutex m_histMutex;

    // byte-value histogram of the raw stream, subsampled every 4th byte
    uint32_t m_hist[256] = {};
    uint64_t m_histCount = 0;

    rtlsdr_dev_t* m_dev = nullptr;
    std::thread   m_thread;
    std::string m_serialString = "";
    bool m_vgaSupported = false;
};
