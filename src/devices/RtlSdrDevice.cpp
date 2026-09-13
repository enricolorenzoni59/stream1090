/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#include "devices/RtlSdrDevice.hpp"
#include "devices/RtlSdrSerial.hpp"
#include "Logger.hpp"
#include "Metrics.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

void RtlSdrDevice::callback(unsigned char* buf, uint32_t len, void* ctx) {
    auto* self = static_cast<RtlSdrDevice*>(ctx);

    if (!self->isRunning())
        return;

    self->markAsAlive();
    self->observeSamples(len);
    self->writeDataToBuffer(buf, len);
}

// ----------------------
// Open
// ----------------------
bool RtlSdrDevice::open_with_serial(const std::string& serial) {
    if (serial.empty()) {
        return open_with_serial(static_cast<uint64_t>(0));
    }

    const int deviceCount = static_cast<int>(rtlsdr_get_device_count());
    if (deviceCount <= 0) {
        Log::error("RtlSdrDevice") << "No supported RTL-SDR devices found.";
        return false;
    }
    std::vector<std::string> available;
    available.reserve(deviceCount > 0 ? static_cast<std::size_t>(deviceCount) : 0);
    for (int i = 0; i < deviceCount; ++i) {
        char deviceSerial[256]{};
        if (rtlsdr_get_device_usb_strings(i, nullptr, nullptr, deviceSerial) == 0)
            available.emplace_back(deviceSerial);
        else
            available.emplace_back();
    }

    const int index = RtlSdrSerial::resolveIndex(serial, available);
    if (index < 0) {
        Log::error("RtlSdrDevice") << "No RTL-SDR device found with serial '" << serial << "'";
        return false;
    }

    const int openRc = rtlsdr_open(&m_dev, index);
    if (openRc != 0) {
        Log::error("RtlSdrDevice") << "rtlsdr_open failed with code " << openRc;
        return false;
    }

    char buf[256];
    rtlsdr_get_device_usb_strings(index, nullptr, nullptr, buf);
    m_actualSerial = std::strtoull(buf, nullptr, 0);

    auto check = [&](const char* name, int rc) {
        if (rc != 0) {
            Log::error("RtlSdrDevice") << "ERROR: " << name << " failed with code " << rc;
            const int closeRc = rtlsdr_close(m_dev);
            if (closeRc != 0)
                Log::error("RtlSdrDevice") << "rtlsdr_close after setup failure returned " << closeRc;
            m_dev = nullptr;
            return false;
        }
        return true;
    };

    // Set the frequency before the sample rate: R820T bandwidth setup retunes
    // the current frequency, and immediately after open() that value is zero.
    if (!check("rtlsdr_set_center_freq", rtlsdr_set_center_freq(m_dev, m_openFrequency)))
        return false;
    m_state.frequency = m_openFrequency;

    if (!check("rtlsdr_set_sample_rate", rtlsdr_set_sample_rate(m_dev, getSampleRate())))
        return false;

    if (!check("rtlsdr_reset_buffer", rtlsdr_reset_buffer(m_dev)))
        return false;
    return true;
}

bool RtlSdrDevice::open_with_serial(uint64_t serial) {
    int deviceCount = rtlsdr_get_device_count();
    if (deviceCount <= 0) {
        Log::error("RtlSdrDevice") << "No supported RTL-SDR devices found.";
        return false;
    }

    int index = 0;

    if (serial != 0) {
        bool found = false;
        for (int i = 0; i < deviceCount; i++) {
            char buf[256];
            rtlsdr_get_device_usb_strings(i, nullptr, nullptr, buf);
            uint64_t devSerial = std::strtoull(buf, nullptr, 0);

            if (devSerial == serial) {
                index = i;
                found = true;
                break;
            }
        }

        if (!found)
            return false;
    }

    const int openRc = rtlsdr_open(&m_dev, index);
    if (openRc != 0) {
        Log::error("RtlSdrDevice") << "rtlsdr_open failed with code " << openRc;
        return false;
    }

    char buf[256];
    rtlsdr_get_device_usb_strings(index, nullptr, nullptr, buf);
    m_actualSerial = std::strtoull(buf, nullptr, 0);

    auto check = [&](const char* name, int rc) {
        if (rc != 0) {
            Log::error("RtlSdrDevice") << "ERROR: " << name << " failed with code " << rc;
            const int closeRc = rtlsdr_close(m_dev);
            if (closeRc != 0)
                Log::error("RtlSdrDevice") << "rtlsdr_close after setup failure returned " << closeRc;
            m_dev = nullptr;
            return false;
        }
        return true;
    };

    // Set the frequency before the sample rate: R820T bandwidth setup retunes
    // the current frequency, and immediately after open() that value is zero.
    if (!check("rtlsdr_set_center_freq", rtlsdr_set_center_freq(m_dev, m_openFrequency)))
        return false;
    m_state.frequency = m_openFrequency;

    if (!check("rtlsdr_set_sample_rate", rtlsdr_set_sample_rate(m_dev, getSampleRate())))
        return false;

    if (!check("rtlsdr_reset_buffer", rtlsdr_reset_buffer(m_dev)))
        return false;
    return true;
}

bool RtlSdrDevice::open() {
    if (!open_with_serial(m_serialString))
        return false;

    const char* tunerName = "unknown";
    switch (rtlsdr_get_tuner_type(m_dev)) {
    case RTLSDR_TUNER_R820T:
        tunerName = "R820T/R820T2";
        break;
    case RTLSDR_TUNER_R828D:
        tunerName = "R828D";
        break;
    case RTLSDR_TUNER_E4000:
        tunerName = "E4000";
        break;
    case RTLSDR_TUNER_FC0012:
        tunerName = "FC0012";
        break;
    case RTLSDR_TUNER_FC0013:
        tunerName = "FC0013";
        break;
    case RTLSDR_TUNER_FC2580:
        tunerName = "FC2580";
        break;
    default:
        break;
    }
    std::cerr << "[RtlSdrDevice] Tuner: " << tunerName << std::endl;
    return true;
}

// ----------------------
// Start / Stop / Close
// ----------------------
bool RtlSdrDevice::start() {
    if (!m_dev)
        return false;

    m_running.store(true, std::memory_order_relaxed);

    m_thread = std::thread([this]() {
        int rc = rtlsdr_read_async(m_dev, callback, this, 0, 0);

        if (rc != 0)
            Log::error("RtlSdrDevice") << "rtlsdr_read_async failed: " << rc;

        m_running.store(false, std::memory_order_relaxed);
    });

    return true;
}

void RtlSdrDevice::stop() {
    m_bufferWriter.shutdown();
    if (!m_dev)
        return;

    m_running.store(false, std::memory_order_relaxed);
    rtlsdr_cancel_async(m_dev);
    if (m_thread.joinable())
        m_thread.join();
}

void RtlSdrDevice::close() {
    stop();
    if (m_dev) {
        rtlsdr_close(m_dev);
        m_dev = nullptr;
    }
}

int RtlSdrDevice::nearestGain(int requested) {
    if (!m_dev)
        return 0;

    int gains[256];
    int count = rtlsdr_get_tuner_gains(m_dev, gains);

    if (count <= 0)
        return 0;

    int best = gains[0];
    int bestDiff = std::abs(requested - best);

    for (int i = 1; i < count; i++) {
        int diff = std::abs(requested - gains[i]);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = gains[i];
        }
    }

    return best;
}

// ----------------------
// Shadow-aware setters with change logging
// ----------------------

bool RtlSdrDevice::setFrequency(uint32_t hz) {
    if (m_state.frequency == hz)
        return true;

    if (rtlsdr_set_center_freq(m_dev, hz) == 0) {
        Log::info("RtlSdrDevice") << "frequency: " << m_state.frequency << " -> " << hz;
        m_state.frequency = hz;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setGain(float gainDb) {
    if (m_state.gain_db == gainDb)
        return true;

    rtlsdr_set_tuner_gain_mode(m_dev, 1);

    int gainTenths = static_cast<int>(gainDb * 10.0f);
    int nearest = nearestGain(gainTenths);

    if (rtlsdr_set_tuner_gain(m_dev, nearest) == 0) {
        Log::info("RtlSdrDevice") << "gain: " << m_state.gain_db << " dB -> " << gainDb << " dB"
                                  << " (nearest step = " << nearest / 10.0f << " dB)";
        m_state.gain_db = gainDb;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setAgc(bool enabled) {
    if (m_state.agc == enabled)
        return true;

    if (rtlsdr_set_agc_mode(m_dev, enabled ? 1 : 0) == 0) {
        Log::info("RtlSdrDevice") << "agc: " << (m_state.agc ? "on" : "off") << " -> " << (enabled ? "on" : "off");
        m_state.agc = enabled;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setBiasTee(bool enabled) {
    if (m_state.bias_tee == enabled)
        return true;

    if (rtlsdr_set_bias_tee(m_dev, enabled ? 1 : 0) == 0) {
        Log::info("RtlSdrDevice") << "bias_tee: " << (m_state.bias_tee ? "on" : "off") << " -> "
                                  << (enabled ? "on" : "off");
        m_state.bias_tee = enabled;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setPpm(int ppm) {
    if (m_state.ppm == ppm)
        return true;

    if (rtlsdr_set_freq_correction(m_dev, ppm) == 0) {
        Log::info("RtlSdrDevice") << "ppm: " << m_state.ppm << " -> " << ppm;
        m_state.ppm = ppm;
        Metrics::registry().settingPpm.set(double(ppm));
        Metrics::registry().rtlAutoPpmCorrection.set(double(ppm));
        resetAutoPpmMeasurement();
        return true;
    }
    return false;
}

void RtlSdrDevice::observeSamples(uint32_t len) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_autoPpmMutex);
    m_autoPpm.totalPairs += len / 2U;
    m_autoPpm.latestSample = now;
    if (!m_autoPpm.haveSample) {
        m_autoPpm.firstSample = now;
        m_autoPpm.haveSample = true;
    }
}

void RtlSdrDevice::resetAutoPpmMeasurement() {
    bool enabled;
    {
        std::lock_guard<std::mutex> lock(m_autoPpmMutex);
        m_autoPpm.haveBaseline = false;
        m_autoPpm.measurements.clear();
        if (m_autoPpm.haveSample)
            m_autoPpm.firstSample = m_autoPpm.latestSample;
        enabled = m_autoPpm.enabled;
    }
    auto& metrics = Metrics::registry();
    metrics.rtlAutoPpmWindowsCollected.set(0.0);
    if constexpr (Metrics::Enabled)
        metrics.rtlAutoPpmPhase.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void RtlSdrDevice::configureAutoPpm(const IniConfig::Section& cfg) {
    bool enabled;
    unsigned interval;
    unsigned warmup;
    unsigned samples;
    int maxStep;
    int deadband;
    int limit;
    {
        std::lock_guard<std::mutex> lock(m_autoPpmMutex);
        enabled = m_autoPpm.enabled;
        interval = m_autoPpm.intervalSeconds;
        warmup = m_autoPpm.warmupSeconds;
        samples = m_autoPpm.samples;
        maxStep = m_autoPpm.maxStep;
        deadband = m_autoPpm.deadband;
        limit = m_autoPpm.limit;
    }

    auto isTrue = [](const std::string& value) {
        return value == "1" || value == "true" || value == "on" || value == "yes";
    };
    if (auto it = cfg.find("auto_ppm"); it != cfg.end())
        enabled = isTrue(it->second);
    if (auto it = cfg.find("auto_ppm_interval"); it != cfg.end())
        interval = std::max(10, std::stoi(it->second));
    if (auto it = cfg.find("auto_ppm_warmup"); it != cfg.end())
        warmup = std::max(0, std::stoi(it->second));
    if (auto it = cfg.find("auto_ppm_samples"); it != cfg.end())
        samples = std::clamp(std::stoi(it->second), 3, 31);
    if (auto it = cfg.find("auto_ppm_max_step"); it != cfg.end())
        maxStep = std::clamp(std::stoi(it->second), 1, 100);
    if (auto it = cfg.find("auto_ppm_deadband"); it != cfg.end())
        deadband = std::clamp(std::stoi(it->second), 0, 20);
    if (auto it = cfg.find("auto_ppm_limit"); it != cfg.end())
        limit = std::clamp(std::stoi(it->second), 1, 1000);

    // Publish the effective (clamped) configuration even when it matches the
    // defaults and therefore does not reset an in-progress measurement.
    auto& metrics = Metrics::registry();
    metrics.rtlAutoPpmEnabled.set(enabled ? 1.0 : 0.0);
    metrics.rtlAutoPpmCorrection.set(double(m_state.ppm));
    metrics.rtlAutoPpmWindowSeconds.set(double(interval));
    metrics.rtlAutoPpmTargetWindows.set(double(samples));
    metrics.rtlAutoPpmWarmupSeconds.set(double(warmup));
    metrics.rtlAutoPpmDeadband.set(double(deadband));
    metrics.rtlAutoPpmMaxStep.set(double(maxStep));
    metrics.rtlAutoPpmLimit.set(double(limit));

    bool changed;
    {
        std::lock_guard<std::mutex> lock(m_autoPpmMutex);
        changed = enabled != m_autoPpm.enabled ||
                  interval != m_autoPpm.intervalSeconds ||
                  warmup != m_autoPpm.warmupSeconds ||
                  samples != m_autoPpm.samples ||
                  maxStep != m_autoPpm.maxStep ||
                  deadband != m_autoPpm.deadband || limit != m_autoPpm.limit;
        if (!changed)
            return;
        m_autoPpm.enabled = enabled;
        m_autoPpm.intervalSeconds = interval;
        m_autoPpm.warmupSeconds = warmup;
        m_autoPpm.samples = samples;
        m_autoPpm.maxStep = maxStep;
        m_autoPpm.deadband = deadband;
        m_autoPpm.limit = limit;
        m_autoPpm.haveBaseline = false;
        m_autoPpm.measurements.clear();
        if (m_autoPpm.haveSample)
            m_autoPpm.firstSample = m_autoPpm.latestSample;
    }

    Log::info("RtlSdrDevice") << "automatic PPM correction: "
        << (enabled ? "on" : "off") << " (warm-up " << warmup
        << " s, median of " << samples << " x " << interval
        << " s, max step " << maxStep
        << " ppm, deadband +/-" << deadband << " ppm, limit +/-" << limit
        << " ppm)";

    metrics.rtlAutoPpmWindowsCollected.set(0.0);
    if constexpr (Metrics::Enabled)
        metrics.rtlAutoPpmPhase.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void RtlSdrDevice::periodicMaintenance() {
    int currentPpm = 0;
    int targetPpm = 0;
    double residualPpm = 0.0;
    double elapsed = 0.0;
    unsigned measurementCount = 0;
    bool apply = false;

    {
        std::lock_guard<std::mutex> lock(m_autoPpmMutex);
        if (!m_autoPpm.enabled || !m_autoPpm.haveSample)
            return;

        if (!m_autoPpm.haveBaseline) {
            if (m_autoPpm.latestSample - m_autoPpm.firstSample <
                    std::chrono::seconds(m_autoPpm.warmupSeconds))
                return;
            m_autoPpm.baselineTime = m_autoPpm.latestSample;
            m_autoPpm.baselinePairs = m_autoPpm.totalPairs;
            m_autoPpm.baselineDropEvents = dropEventCount();
            m_autoPpm.haveBaseline = true;
            if constexpr (Metrics::Enabled)
                Metrics::registry().rtlAutoPpmPhase.store(2, std::memory_order_relaxed);
            Log::info("RtlSdrDevice") << "automatic PPM warm-up complete; measurement started.";
            return;
        }

        elapsed = std::chrono::duration<double>(
            m_autoPpm.latestSample - m_autoPpm.baselineTime).count();
        if (elapsed < m_autoPpm.intervalSeconds)
            return;

        const uint64_t pairs = m_autoPpm.totalPairs - m_autoPpm.baselinePairs;
        const bool dropped = dropEventCount() != m_autoPpm.baselineDropEvents;
        m_autoPpm.baselineTime = m_autoPpm.latestSample;
        m_autoPpm.baselinePairs = m_autoPpm.totalPairs;
        m_autoPpm.baselineDropEvents = dropEventCount();

        if (dropped) {
            Metrics::registry().rtlAutoPpmWindowsDiscarded.inc();
            Log::warn("RtlSdrDevice")
                << "automatic PPM measurement discarded because samples were dropped.";
            return;
        }

        const double observation = 1.0e6 *
            ((static_cast<double>(pairs) / elapsed) /
             static_cast<double>(getSampleRate()) - 1.0);
        m_autoPpm.measurements.push_back(observation);
        auto& metrics = Metrics::registry();
        metrics.rtlAutoPpmWindowsClean.inc();
        metrics.rtlAutoPpmLastObservation.set(observation);
        metrics.rtlAutoPpmSampleRateHz.set(static_cast<double>(pairs) / elapsed);
        metrics.rtlAutoPpmWindowsCollected.set(double(m_autoPpm.measurements.size()));
        if (m_autoPpm.measurements.size() < m_autoPpm.samples)
            return;

        // Callback completion timestamps occasionally contain large USB or
        // scheduler outliers. Independent windows plus a median reject them.
        auto values = m_autoPpm.measurements;
        const auto middle = values.begin() + values.size() / 2;
        std::nth_element(values.begin(), middle, values.end());
        residualPpm = *middle;
        measurementCount = static_cast<unsigned>(values.size());
        m_autoPpm.measurements.clear();
        currentPpm = m_state.ppm;
        metrics.rtlAutoPpmMedianResidual.set(residualPpm);
        metrics.rtlAutoPpmEstimatedError.set(double(currentPpm) + residualPpm);
        metrics.rtlAutoPpmLastEstimateSteady.set(Metrics::Registry::steadySeconds());
        metrics.rtlAutoPpmWindowsCollected.set(0.0);
        int requestedStep = static_cast<int>(std::lround(residualPpm));
        if (std::abs(requestedStep) <= m_autoPpm.deadband)
            requestedStep = 0;
        const int boundedStep = std::clamp(requestedStep,
                                           -m_autoPpm.maxStep,
                                           m_autoPpm.maxStep);
        targetPpm = std::clamp(currentPpm + boundedStep,
                               -m_autoPpm.limit, m_autoPpm.limit);
        apply = targetPpm != currentPpm;
    }

    Log::info("RtlSdrDevice") << "automatic PPM: median residual " << residualPpm
        << " ppm from " << measurementCount << " clean windows; correction "
        << currentPpm
        << (apply ? " -> " : " retained at ") << targetPpm;
    if (apply) {
        if (setPpm(targetPpm)) {
            Metrics::registry().rtlAutoPpmDecisionsApplied.inc();
        } else {
            Metrics::registry().rtlAutoPpmDecisionsFailed.inc();
            Log::warn("RtlSdrDevice") << "automatic PPM correction failed.";
        }
    } else {
        Metrics::registry().rtlAutoPpmDecisionsHeld.inc();
    }
}

bool RtlSdrDevice::setOffsetTuning(bool enabled) {
    if (m_state.offset_tuning == enabled)
        return true;

    if (rtlsdr_set_offset_tuning(m_dev, enabled ? 1 : 0) == 0) {
        Log::info("RtlSdrDevice") << "offset_tuning: " << (m_state.offset_tuning ? "on" : "off") << " -> "
                                  << (enabled ? "on" : "off");
        m_state.offset_tuning = enabled;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setTunerBandwidth(uint32_t bw) {
    if (m_state.tuner_bandwidth == bw)
        return true;

    if (rtlsdr_set_tuner_bandwidth(m_dev, bw) == 0) {
        Log::info("RtlSdrDevice") << "tuner_bandwidth: " << m_state.tuner_bandwidth << " -> " << bw;
        m_state.tuner_bandwidth = bw;
        return true;
    }
    return false;
}

#ifdef STREAM1090_HAVE_RTLSDR_BLOG
bool RtlSdrDevice::setLnaGain(int gain) {
    if (!m_dev)
        return false;

    // Shadow awareness
    if (m_state.lna_gain == gain)
        return true;

    if (rtlsdr_r82xx_set_lna_gain(m_dev, gain) != 0)
        return false;

    Log::info("RtlSdrDevice") << "LNA gain: " << m_state.lna_gain << " -> " << gain;

    m_state.lna_gain = gain;
    return true;
}

bool RtlSdrDevice::setMixerGain(int gain) {
    if (!m_dev)
        return false;

    // Shadow awareness
    if (m_state.mixer_gain == gain)
        return true;

    if (rtlsdr_r82xx_set_mixer_gain(m_dev, gain) != 0)
        return false;

    Log::info("RtlSdrDevice") << "Mixer gain: " << m_state.mixer_gain << " -> " << gain;

    m_state.mixer_gain = gain;
    return true;
}

bool RtlSdrDevice::setVgaGain(int gain) {
    if (!m_dev)
        return false;

    // Shadow awareness
    if (m_state.vga_gain == gain)
        return true;

    if (rtlsdr_r82xx_set_vga_gain(m_dev, gain) != 0)
        return false;

    Log::info("RtlSdrDevice") << "VGA gain: " << m_state.vga_gain << " -> " << gain;

    m_state.vga_gain = gain;
    return true;
}

#else
bool RtlSdrDevice::setLnaGain(int) {
    return false;
}
bool RtlSdrDevice::setMixerGain(int) {
    return false;
}
bool RtlSdrDevice::setVgaGain(int) {
    return false;
}
#endif

RtlSdrDevice::GainState RtlSdrDevice::gainState() const {
    GainState state;
    state.db = true;
    state.autoGain = m_state.agc;
    state.mode = m_state.agc ? GainState::ModeAuto : GainState::ModeTuner;
    state.overall = m_state.gain_db;
#ifdef STREAM1090_HAVE_RTLSDR_BLOG
    // The per-stage controls only exist in the vendored rtl-sdr-blog fork.
    state.hasStages = true;
    state.lna = float(m_state.lna_gain);
    state.mixer = float(m_state.mixer_gain);
    state.vga = float(m_state.vga_gain);
#else
    state.hasStages = false;
#endif
    return state;
}

// ----------------------
// applySetting()
// ----------------------
bool RtlSdrDevice::applySetting(const std::string& key, const std::string& value) {
    if (!m_dev)
        return false;

    // Core controls
    if (key == "frequency")
        return setFrequency(std::stoul(value));
    if (key == "gain")
        return setGain(std::stof(value));
    if (key == "agc")
        return setAgc(value == "1" || value == "true" || value == "on");
    if (key == "bias_tee")
        return setBiasTee(value == "1" || value == "true" || value == "on");
    if (key == "ppm")
        return setPpm(std::stoi(value));
    if (key == "offset_tuning")
        return setOffsetTuning(value == "1" || value == "true" || value == "on");
    if (key == "tuner_bandwidth")
        return setTunerBandwidth(std::stoul(value));

    // Advanced per‑stage gain controls (R820T manual mode)
    if (key == "lna_gain")
        return setLnaGain(std::stoi(value));
    if (key == "mixer_gain")
        return setMixerGain(std::stoi(value));
    if (key == "vga_gain")
        return setVgaGain(std::stoi(value));

    return false;
}

void RtlSdrDevice::applyConfigPreOpen(const IniConfig::Section& cfg) {
    for (auto& [key, value] : cfg) {

        if (key == "serial")
            m_serialString = value;
        else if (key == "frequency")
            m_openFrequency = static_cast<uint32_t>(std::stoul(value));
    }
}

// ----------------------
// Reload logic
// ----------------------
void RtlSdrDevice::applyConfigPostOpen(const IniConfig::Section& cfg) {
    configureAutoPpm(cfg);
    if (!m_initialConfigApplied) {
        m_initialConfigApplied = true;

        if (!cfg.count("tuner_bandwidth") && rtlsdr_get_tuner_type(m_dev) == RTLSDR_TUNER_R820T) {
            Log::warn("RtlSdrDevice") << "No tuner_bandwidth configured for this R820T/R820T2 tuner; "
                                         "automatic IF filter selection depends on the sample rate and "
                                         "librtlsdr implementation. Set it explicitly (for example, "
                                         "3000000 at 2.4 or 2.56 Msps) to make the tuner state reproducible.";
        }
    }

    for (auto& [key, value] : cfg) {

        if (key == "serial" || key == "auto_ppm" ||
            key == "auto_ppm_interval" || key == "auto_ppm_warmup" ||
            key == "auto_ppm_samples" || key == "auto_ppm_max_step" ||
            key == "auto_ppm_deadband" ||
            key == "auto_ppm_limit")
            continue;

        applySetting(key, value);
    }

    // Report the bandwidth setting once. librtlsdr has no read-back API for
    // the effective bandwidth it derives when tuner_bandwidth is omitted.
    if (!m_stateReported) {
        m_stateReported = true;
        std::cerr << "[RtlSdrDevice] Tuner bandwidth setting: "
                  << (m_state.tuner_bandwidth ? std::to_string(m_state.tuner_bandwidth) + " Hz (explicit)"
                                              : std::string("auto (derived by librtlsdr from sample rate)"))
                  << std::endl;
    }
}
