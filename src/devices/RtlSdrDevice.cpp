/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#include "devices/RtlSdrDevice.hpp"
#include "devices/RtlSdrSerial.hpp"
#include "AdaptiveGainLogic.hpp"
#include "AdcHistogramDiagnostics.hpp"
#include "Counters.hpp"
#include "Logger.hpp"
#include "Metrics.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef STREAM1090_HAVE_RTLSDR_BLOG
// The vendored fork reports its messages through this callback instead of
// writing straight to stderr, so tuner detection, PLL failures and the rest
// carry the same timestamp and level as the rest of the log. See the
// rtlsdr_log patch in thirdparty/rtl-sdr-blog.
static void forwardRtlsdrLog(rtlsdr_log_level_t level, const char* message) {
    // The library messages end with a newline; the logger adds its own, so
    // strip one to avoid a blank line after every entry.
    std::string text(message);
    if (!text.empty() && text.back() == '\n')
        text.pop_back();

    switch (level) {
    case RTLSDR_LOG_ERROR:
        Log::error("librtlsdr") << text;
        break;
    case RTLSDR_LOG_WARN:
        Log::warn("librtlsdr") << text;
        break;
    case RTLSDR_LOG_DEBUG:
        Log::debug("librtlsdr") << text;
        break;
    default:
        Log::info("librtlsdr") << text;
        break;
    }
}
#endif

void RtlSdrDevice::callback(unsigned char* buf, uint32_t len, void* ctx) {
    auto* self = static_cast<RtlSdrDevice*>(ctx);

    if (!self->isRunning())
        return;

    self->markAsAlive();
    self->observeSamples(len);
    // The histogram feeds the adaptive gain loop and its diagnostics.
    self->accumulateGainStats(buf, len);
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
#ifdef STREAM1090_HAVE_RTLSDR_BLOG
    rtlsdr_set_log_callback(forwardRtlsdrLog);
#endif
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
    Log::msg("RtlSdrDevice") << "Tuner: " << tunerName;
#ifdef STREAM1090_HAVE_RTLSDR_BLOG
    // The per-stage setters (and so the VGA knob the loop climbs with) exist
    // only in the vendored fork, and only for the r82xx family.
    const auto tuner = rtlsdr_get_tuner_type(m_dev);
    m_vgaSupported = tuner == RTLSDR_TUNER_R820T || tuner == RTLSDR_TUNER_R828D;
#else
    m_vgaSupported = false;
#endif
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
        const int rc = rtlsdr_read_async(m_dev, callback, this, 0, 0);

        if (rc != 0) {
            // A non-zero return on our own shutdown (cancel on SIGINT) is
            // expected: keep it out of the default log. A non-zero return
            // while we are not stopping means the reader stopped on its own,
            // usually because the device went away; the watchdog reports that.
            if (m_stopping.load(std::memory_order_relaxed))
                Log::debug("RtlSdrDevice") << "rtlsdr_read_async ended with " << rc << " during shutdown";
            else
                Log::warn("RtlSdrDevice") << "rtlsdr_read_async returned " << rc;
        }

        m_running.store(false, std::memory_order_relaxed);
    });

    startAdaptiveGain();
    return true;
}

void RtlSdrDevice::stop() {
    m_bufferWriter.shutdown();
    if (!m_dev)
        return;

    m_stopping.store(true, std::memory_order_relaxed);
    stopAdaptiveGain();
    m_running.store(false, std::memory_order_relaxed);
    rtlsdr_cancel_async(m_dev);
    if (m_thread.joinable())
        m_thread.join();
}

void RtlSdrDevice::close() {
    stop();
    if (m_dev) {
#ifdef STREAM1090_HAVE_RTLSDR_BLOG
        // Tell the library the device is gone so rtlsdr_close takes its
        // dev_lost path and skips the tuner deinit. On a device that was
        // unplugged every register write in there fails with
        // LIBUSB_ERROR_NO_DEVICE, which is what the watchdog already knows.
        if (m_deviceLost)
            rtlsdr_mark_dev_lost(m_dev);
#endif
        rtlsdr_close(m_dev);
        m_dev = nullptr;
    }
    m_state.gain_known = false;
}

void RtlSdrDevice::markDeviceLost() {
    m_deviceLost = true;
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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
    const int nearest = nearestGain(static_cast<int>(std::lround(gainDb * 10.0f)));
    // The shadow holds the table step actually applied, not the request: the
    // adaptive loop walks the table from it.
    if (m_state.gain_known && std::lround(m_state.gain_db * 10.0f) == nearest)
        return true;

    if (applyManualTunerGain([&] { return rtlsdr_set_tuner_gain_mode(m_dev, 1); },
                             [&] { return rtlsdr_set_tuner_gain(m_dev, nearest); })) {
        Log::info("RtlSdrDevice") << "gain: " << m_state.gain_db << " dB -> " << gainDb << " dB"
                                  << " (nearest step = " << nearest / 10.0f << " dB)";
        recordLinearGain(nearest);
        return true;
    }
    return false;
}

// r82xx per-step gains in tenths of dB, from the vendored tuner_r82xx.c: the
// "linear" gain walks LNA and MIXER alternately until the requested total,
// with the VGA fixed at index 8 (16.3 dB).
static constexpr int kLnaSteps[] = {0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13};
static constexpr int kMixSteps[] = {0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8};
static constexpr int kVgaLinearIdx = 8;

static void r82xxWalkPartition(int gainTenths, int& lnaIdx, int& mixIdx) {
    int total = 0;
    lnaIdx = mixIdx = 0;
    for (int i = 0; i < 15; i++) {
        if (total >= gainTenths)
            break;
        total += kLnaSteps[++lnaIdx];
        if (total >= gainTenths)
            break;
        total += kMixSteps[++mixIdx];
    }
}

// Shadow bookkeeping after a combined tuner gain was applied through the
// linear path: it chooses the LNA/MIX split itself and resets the VGA.
void RtlSdrDevice::recordLinearGain(int gainTenths) {
    m_state.gain_db = gainTenths / 10.0f;
    m_state.gain_known = true;
    r82xxWalkPartition(gainTenths, m_state.lna_gain, m_state.mixer_gain);
    m_state.vga_gain = kVgaLinearIdx;
}

bool RtlSdrDevice::applyVgaGain(int value) {
#ifdef STREAM1090_HAVE_RTLSDR_BLOG
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
    if (!m_vgaSupported || value < 0 || value > 15)
        return false;
    if (rtlsdr_r82xx_set_vga_gain(m_dev, value) != 0)
        return false;
    m_state.vga_gain = value;
    return true;
#else
    (void)value;
    return false;
#endif
}

bool RtlSdrDevice::setAgc(bool enabled) {
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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

void RtlSdrDevice::configureAutoPpm(const AutoPpmConfig& cfg) {
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

    enabled = cfg.enabled;
    interval = static_cast<unsigned>(std::max(10, static_cast<int>(cfg.intervalSeconds)));
    warmup = static_cast<unsigned>(std::max(0, static_cast<int>(cfg.warmupSeconds)));
    samples = static_cast<unsigned>(std::clamp(static_cast<int>(cfg.samples), 3, 31));
    maxStep = std::clamp(cfg.maxStep, 1, 100);
    deadband = std::clamp(cfg.deadband, 0, 20);
    limit = std::clamp(cfg.limit, 1, 1000);

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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
    // m_state.tuner_bandwidth starts at 0, which is also the value that asks
    // librtlsdr to derive the bandwidth: without the flag a configured "0"
    // would match the initial state and never reach the driver.
    if (m_bandwidthApplied && m_state.tuner_bandwidth == bw)
        return true;

    if (rtlsdr_set_tuner_bandwidth(m_dev, bw) == 0) {
        Log::info("RtlSdrDevice") << "tuner_bandwidth: " << m_state.tuner_bandwidth << " -> " << bw;
        m_state.tuner_bandwidth = bw;
        m_bandwidthApplied = true;
        return true;
    }
    return false;
}

#ifdef STREAM1090_HAVE_RTLSDR_BLOG
bool RtlSdrDevice::setLnaGain(int gain) {
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
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
void RtlSdrDevice::applyConfigPreOpen(const DeviceConfig& cfg) {
    if (cfg.serial)
        m_serialString = *cfg.serial;
    else
        m_serialString.clear();
    m_openFrequency = cfg.frequencyHz;
}

// ----------------------
// Reload logic
// ----------------------
void RtlSdrDevice::applyConfigPostOpen(const DeviceConfig& cfg) {
    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
    configureAutoPpm(cfg.autoPpm);
    if (!m_initialConfigApplied) {
        m_initialConfigApplied = true;

        // applyBackendDefaults() always engages the optional for RTL-SDR, so
        // testing it for emptiness never fired. What matters is the effective
        // value: 0 means librtlsdr picks the filter.
        if (cfg.tunerBandwidth.value_or(0) == 0 && rtlsdr_get_tuner_type(m_dev) == RTLSDR_TUNER_R820T) {
            Log::warn("RtlSdrDevice") << "No tuner_bandwidth configured for this R820T/R820T2 tuner; "
                                         "automatic IF filter selection depends on the sample rate and "
                                         "librtlsdr implementation. Set it explicitly (for example, "
                                         "3000000 at 2.4 or 2.56 Msps) to make the tuner state reproducible.";
        }
    }

    setFrequency(cfg.frequencyHz);
    setAgc(cfg.agc);
    if (cfg.gainDb)
        setGain(*cfg.gainDb);
    if (cfg.tunerBandwidth)
        setTunerBandwidth(*cfg.tunerBandwidth);
    setBiasTee(cfg.biasTee);
    setOffsetTuning(cfg.offsetTuning);
    if (cfg.ppm)
        setPpm(*cfg.ppm);

    if (cfg.lnaGain)
        setLnaGain(*cfg.lnaGain);
    if (cfg.mixerGain)
        setMixerGain(*cfg.mixerGain);
    if (cfg.vgaGain)
        setVgaGain(*cfg.vgaGain);

    const bool pinned = !cfg.adaptiveGain || cfg.agc;
    if (m_gainPinned.exchange(pinned) != pinned || !m_stateReported)
        Log::msg("RtlSdrDevice") << "Gain control: "
                                 << (pinned ? "pinned at " : "adaptive, starting from ")
                                 << m_state.gain_db << " dB (LNA/MIX/VGA " << m_state.lna_gain << '/'
                                 << m_state.mixer_gain << '/' << m_state.vga_gain << ")";

    // Report the bandwidth setting once. librtlsdr has no read-back API for
    // the effective bandwidth it derives when tuner_bandwidth is omitted.
    if (!m_stateReported) {
        m_stateReported = true;
        Log::msg("RtlSdrDevice") << "Tuner bandwidth setting: "
                                 << (m_state.tuner_bandwidth
                                         ? std::to_string(m_state.tuner_bandwidth) + " Hz (explicit)"
                                         : std::string("auto (derived by librtlsdr from sample rate)"));
    }
}

// ----------------------
// Adaptive gain
// ----------------------
void RtlSdrDevice::accumulateGainStats(const unsigned char* buf, uint32_t len) {
    // Two complete I/Q pairs out of every eight: the callback still counts
    // one byte in four, but the histogram represents both ADC channels.
    std::lock_guard<std::mutex> lock(m_histMutex);
    m_histCount += accumulateSampledAdcHistogram(buf, len, m_hist);
}

void RtlSdrDevice::startAdaptiveGain() {
    if (m_adaptiveRun.load(std::memory_order_relaxed))
        return;
    m_adaptiveRun.store(true, std::memory_order_relaxed);
    m_adaptiveThread = std::thread([this]() { adaptiveGainLoop(); });
}

void RtlSdrDevice::stopAdaptiveGain() {
    m_adaptiveRun.store(false, std::memory_order_relaxed);
    if (m_adaptiveThread.joinable())
        m_adaptiveThread.join();
}

void RtlSdrDevice::adaptiveGainLoop() {
    // one window = 5 s of subsampled stream; decisions are one r82xx
    // step (~2 dB) per window, three when the tuner is in hard overload
    const int evalEveryTicks = 5;
    // floor setpoint: median noise radius in LSB. The 2.4 Msps frame-rate
    // optimum sits at p50 = 0.5..2.5 LSB; below 1 the
    // quantization starts costing frames, above ~3.5 the headroom to the
    // rails shrinks without any frame benefit.
    constexpr double kSigmaLow = 2.5;   // below: digital noise eats the signal
    constexpr double kSigmaHigh = 5.0;  // above: headroom to the rails wasted
    // Where in the band the loop actually wants to sit. Stopping at the
    // first in-band window parks it wherever it crossed the edge, and the
    // band is wider than a step: see climbClosesOnTarget(). The geometric
    // centre puts the worst-case rest point half a step away instead of a
    // whole band away.
    const double kSigmaTarget = std::sqrt(kSigmaLow * kSigmaHigh);
    constexpr double kHardOverload = 30.0;
    // a climb that lifts the RMS by less than this did not respond:
    // what the tuner is amplifying is no longer the analog floor
    constexpr double kClimbResponseDb = 0.75;
    // share of |x| <= 1 LSB above which the median is pinned by
    // discreteness and no amount of gain can move the setpoint
    constexpr double kCenterStarved = 85.0;
    // an analog floor this low is not a quiet band, it is an open
    // connector: measured 0.86 and 0.88 LSB on the two cable events
    // of the 2026-09-05 bench, against 4.5..14 LSB in operation
    constexpr double kSilentRms = 1.5;
    // the cable events last one window; a silent floor that persists is
    // not that transient. Absorb a few windows, then treat the empty
    // floor as a real, quiet input and let the loop climb again.
    constexpr int kDropoutWindowsBeforeDecide = 3;
    // the VGA index the linear path fixes (16.3 dB): the digital-floor
    // climb may push it above this, the rebalance paths walk it back
    constexpr int VGA_LINEAR_IDX = kVgaLinearIdx;

    int ticks = 0;
    uint64_t windowHist[256] = {};
    uint64_t windowCount = 0;
    uint64_t prevHist[256];
    uint64_t prevCount = 0;
    bool havePrev = false;
    uint64_t diagnosticHist[256] = {};
    int diagnosticWindows = 0;
    uint64_t lastShortFrames = StreamCounters::decodedShort.load(std::memory_order_relaxed);
    uint64_t lastLongFrames = StreamCounters::decodedLong.load(std::memory_order_relaxed);
    double prevP50 = 0;
    int mudWindows = 0;
    // RMS measured just before the last climb that actually moved the
    // tuner, and whether that climb is still waiting to be scored
    double climbRms = 0.0;
    bool awaitingClimb = false;
    // band-edge confirmation: one window on one side of the setpoint is
    // not enough to move the gain (the morning bench dithered 14.4<->16.6
    // with sigma swinging across the 2.5 LSB edge between windows); two
    // consecutive windows on the same side do step
    int mudStreak = 0;
    int hotStreak = 0;
    // the VGA index under adaptive control: the linear path pins it at 8,
    // but in the digital-floor regime, once the combined gain reaches the
    // mechanical top, the VGA becomes the climb knob (measured 2026-09-05:
    // +12 dB of level above the 49.6 dB top with the frames still flat and
    // the ADC quiet)
    int vgaIdx;
    {
        std::lock_guard<std::recursive_mutex> control(m_controlMutex);
        vgaIdx = m_state.vga_gain;
    }
    int silentWindows = 0;
    bool persistentSilence = false;
    // What the last climb did to the floor, in dB, and the floor it started
    // from. The nominal step overstates the move while quantisation carries
    // part of the floor, so the prediction uses the measurement.
    double stepResponseDb = 2.0;
    double pendingClimbFloor = 0.0;
    int lowStreak = 0;

    while (m_adaptiveRun.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // snapshot the live histogram (callback thread keeps writing into
        // it; a handful of torn increments is irrelevant for percentiles)
        uint32_t snap[256];
        uint64_t snapCount;
        {
            std::lock_guard<std::mutex> lock(m_histMutex);
            memcpy(snap, m_hist, sizeof(snap));
            snapCount = m_histCount;
        }
        if (snapCount < 200000)
            continue; // the device is not streaming yet

        uint64_t h[256];
        if (snapCount < prevCount) {
            memset(prevHist, 0, sizeof(prevHist));
            memcpy(prevHist, snap, sizeof(snap));
            prevCount = snapCount;
            havePrev = false;
            ticks = 0;
            diagnosticWindows = 0;
            memset(diagnosticHist, 0, sizeof(diagnosticHist));
            continue;
        }
        for (int i = 0; i < 256; i++) {
            h[i] = havePrev ? adaptiveHistogramDelta(snap[i], prevHist[i]) : snap[i];
            prevHist[i] = snap[i];
        }
        const uint64_t total = snapCount - prevCount;
        prevCount = snapCount;
        if (!havePrev) {
            havePrev = true;
            continue; // first window only establishes the baseline
        }
        if (total < 200000) {
            // Do not combine samples from before and after a stream pause.
            ticks = 0;
            memset(windowHist, 0, sizeof(windowHist));
            windowCount = 0;
            diagnosticWindows = 0;
            memset(diagnosticHist, 0, sizeof(diagnosticHist));
            continue;
        }
        for (int i = 0; i < 256; ++i)
            windowHist[i] += h[i];
        windowCount += total;
        if (++ticks < evalEveryTicks)
            continue;
        ticks = 0;
        memcpy(h, windowHist, sizeof(h));
        const uint64_t evaluatedTotal = windowCount;
        memset(windowHist, 0, sizeof(windowHist));
        windowCount = 0;

        // metrics from the delta histogram, all ADC-anchored
        uint64_t rails = 0, sumsq = 0, center = 0;
        for (int b = 0; b < 256; b++) {
            const int64_t d = b - 128; // |x| = |byte - 127.5| ~= |b - 128|
            rails += (d <= -126 || d >= 126) ? h[b] : 0;
            sumsq += h[b] * d * d;
            if (d >= -1 && d <= 1)
                center += h[b];
        }
        const double sat = 100.0 * (double)rails / (double)evaluatedTotal;
        const double rms = std::sqrt((double)sumsq / (double)evaluatedTotal);
        // median |x|: walk outward from the center half plane
        // Interpolated inside the crossing ring. The bare ring index is an
        // integer, which leaves sigma_floor only two values inside the band
        // (p50 = 2 and 3, i.e. 2.97 and 4.45): the 2026-09-05 bench removed
        // 14 dB of input and the loop settled one level away from where it
        // had been, because it could not resolve anything finer. Treating
        // the ring as covering [r-0.5, r+0.5] keeps the old value at the
        // midpoint, so the setpoint does not move, and makes the variable
        // continuous everywhere else.
        double p50 = 0;
        {
            const double half = 0.5 * (double)evaluatedTotal;
            double seen = 0;
            for (int r = 0; r <= 128; r++) {
                const uint64_t ring = (128 - r >= 0 ? h[128 - r] : 0) +
                                      (128 + r < 256 ? h[128 + r] : 0) -
                                      (r == 0 ? h[128] : 0);
                const double below = seen;
                seen += (double)ring;
                if (seen >= half) {
                    p50 = (ring > 0)
                        ? (double)r - 0.5 + (half - below) / (double)ring
                        : (double)r;
                    if (p50 < 0.0)
                        p50 = 0.0;
                    break;
                }
            }
        }
        // fraction of samples inside |x| <= 1 LSB: the quantization
        // starvation detector. When the receiver floor drops under ~0.7
        // LSB of sigma, samples pile up at |x| = 0/1 (fraction > 0.85)
        // and quantization noise adds 10%+ to the noise power. The
        // median cannot see this: it saturates at p50 = 1 by discreteness.
        const double centerFrac = 100.0 * (double)center / (double)evaluatedTotal;

        // The floor median is the control variable: it is immune to
        // strong-burst duty cycles below 50% and tracks the environment
        // one to one (attenuate the input by X dB and the loop answers
        // with X dB more tuner gain, within one step). sigma_floor
        // (= p50 / 0.6745 for the half-normal) is the noise the decoder
        // sees in LSB: both measured frame-rate optima (with and without
        // LNA) sit at sigma_floor = 3..5 LSB, where the signal is well
        // above the fixed digital noise floor and the rails are far.
        //
        // Windup guard: below the digital floor, p50 saturates at 1 LSB
        // (discreteness) and sigma_floor reads a constant ~1.48 however
        // much gain is added - the setpoint becomes unreachable and the
        // loop would climb to the end of the scale. The detector is the
        // RMS response to the last climb: while the analog floor
        // dominates, +2 dB of gain raises the RMS by ~2 dB; once the
        // digital floor dominates, the RMS stops responding (< +0.75 dB).
        // Two non-responding climbs in a row declare the setpoint
        // unreachable; the loop holds, and any other decision re-arms it.
        const double sigmaFloor = p50 / 0.6745;

        // Aggregate six control windows, then print a compact 30-second
        // diagnostic. The histogram is of raw 8-bit I/Q codes, sampled by the
        // callback at one byte in four.
        auto reportDiagnostics = [&](const char* reason) {
            for (int code = 0; code < 256; ++code)
                diagnosticHist[code] += h[code];
            if (++diagnosticWindows < 6)
                return;

            const auto summary = summarizeAdcHistogram(diagnosticHist);
            ShadowState state;
            {
                std::lock_guard<std::recursive_mutex> control(m_controlMutex);
                state = m_state;
            }
            const uint64_t shortFrames = StreamCounters::decodedShort.load(std::memory_order_relaxed);
            const uint64_t longFrames = StreamCounters::decodedLong.load(std::memory_order_relaxed);
            std::ostringstream line;
            line << std::fixed << std::setprecision(1)
                 << "30s gain_at_report=" << state.gain_db << "dB"
                 << " LNA/MIX/VGA=" << state.lna_gain << '/'
                 << state.mixer_gain << '/' << state.vga_gain
                 << " frames_short/long=" << (shortFrames - lastShortFrames) << '/'
                 << (longFrames - lastLongFrames)
                 << " drop_events_total=" << dropEventCount()
                 << " adc_samples=" << summary.samples
                 << " rms=" << summary.rmsLsb << "LSB"
                 << " center=" << summary.centerPercent << '%'
                 << " rails=" << summary.railPercent << '%'
                 << " abs_p99=" << summary.absP99Lsb
                 << " abs_p99.9=" << summary.absP999Lsb
                 << " decision=" << reason;
            Log::msg("RtlSdrDiagnostics") << line.str();

            std::ostringstream bins;
            bins << std::fixed << std::setprecision(1)
                 << "ADC 0..255, 16-code bins (%)=";
            for (int bin = 0; bin < 16; ++bin) {
                if (bin)
                    bins << ',';
                bins << (summary.samples > 0
                    ? 100.0 * summary.bins[bin] / summary.samples : 0.0);
            }
            Log::info("RtlSdrDiagnostics") << bins.str();

            memset(diagnosticHist, 0, sizeof(diagnosticHist));
            diagnosticWindows = 0;
            lastShortFrames = shortFrames;
            lastLongFrames = longFrames;
        };

        // Input dropout guard. Pulling or inserting anything in the feed
        // opens the connector for a moment, and the window that catches it
        // reads an empty floor: the loop then answers a cable, not the
        // band. Both cable events of the 2026-09-05 bench did exactly this
        // and cost a step in the wrong direction. Publish the window so it
        // stays visible, leave any pending climb pending, and decide
        // nothing: prevP50 is deliberately not updated either. A silence
        // that outlasts kDropoutWindowsBeforeDecide is not that transient:
        // the loop resumes deciding so the gain can chase the quiet floor.
        const SilentFloorState silence = advanceSilentFloor(
            silentWindows, kDropoutWindowsBeforeDecide, rms, sat, kSilentRms);
        if (silence == SilentFloorState::Transient) {
            Log::info("RtlSdrDevice") << "adaptive gain: input dropout (rms="
                      << rms << " sat=" << sat << "%), window skipped";
            reportDiagnostics("input dropout, holding");
            continue;
        }
        persistentSilence = (silence == SilentFloorState::Persistent);

        // Windup guard, first half: score the previous climb. Two
        // non-responding climbs in a row declare the setpoint
        // unreachable and the loop holds until another decision
        // re-arms it.
        if (awaitingClimb) {
            const double responseDb = (climbRms > 0.0 && rms > 0.0)
                ? 20.0 * std::log10(rms / climbRms) : 0.0;
            // while the floor is quantisation-starved the RMS barely moves
            // per step, so a weak response is not evidence of windup. Only
            // count it once the loop is past the persistent-silence hold.
            if (responseDb < kClimbResponseDb && !persistentSilence)
                mudWindows++;
            else
                mudWindows = 0;
            awaitingClimb = false;
        }

        // Score what the last climb did to the floor itself. Separate from
        // the windup guard above, which scores the RMS and only for tuner
        // steps: the VGA climbs need the same measurement.
        if (pendingClimbFloor > 0.0 && sigmaFloor > 0.0) {
            const double moved = 20.0 * std::log10(sigmaFloor / pendingClimbFloor);
            if (moved > 0.25)
                stepResponseDb = moved;
            pendingClimbFloor = 0.0;
        }

        // Pinned: publish the window and decide nothing. Every actuating
        // branch below is skipped, including the VGA walk-backs.
        if (m_gainPinned.load(std::memory_order_relaxed)) {
            Log::info("RtlSdrDevice") << "adaptive gain: sat=" << sat << "% rms="
                      << rms << " p50=" << p50 << " sigma_floor=" << sigmaFloor
                      << " -> pinned";
            reportDiagnostics("pinned");
            prevP50 = p50;
            continue;
        }

        int steps = 0;
        bool targetClimb = false;
        const char* why = "in band";
        if (p50 > kHardOverload && prevP50 > kHardOverload) {
            steps = -3; why = "sustained hard overload";
            mudWindows = 0;          // any other decision re-arms the guard
            mudStreak = 0;
            hotStreak = 0;
        } else if (sigmaFloor > kSigmaHigh) {
            hotStreak++;
            mudStreak = 0;
            if (hotStreak >= 2) {
                if (vgaIdx > VGA_LINEAR_IDX) {
                    const int candidate = vgaIdx - 1;
                    why = "floor too hot: VGA back";
                    Log::msg("RtlSdrDevice") << "adaptive gain: VGA -> " << candidate
                              << " (floor too hot)";
                    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
                    if (applyVgaGain(candidate))
                        vgaIdx = candidate;
                    else {
                        steps = -1;
                        why = "floor too hot";
                    }
                } else {
                    steps = -1;
                    why = "floor too hot";
                }
                mudWindows = 0;
            } else {
                why = "floor too hot (confirming)";
            }
        } else if (sigmaFloor < kSigmaLow) {
            // In this regime the digital floor hides the analog one and
            // the floor metrics cannot steer: climb while the ADC stays
            // quiet (sat < 0.3%) and hold when the rails warm up. The
            // frame-rate optimum here is at the highest gain the ADC
            // tolerates (measured: monotonic in gain up to 49.6 with a
            // digital-floor front end), so saturation IS the signal.
            mudStreak++;
            hotStreak = 0;
            if (sat > 0.3) {
                if (vgaIdx > VGA_LINEAR_IDX && m_vgaSupported) {
                    // the headroom was bought with VGA: give it back
                    // before touching the combined gain
                    const int candidate = vgaIdx - 1;
                    std::lock_guard<std::recursive_mutex> control(m_controlMutex);
                    if (applyVgaGain(candidate)) {
                        vgaIdx = candidate;
                        why = "rails warming: VGA back";
                    } else {
                        why = "rails warming: VGA adjustment failed";
                    }
                } else {
                    steps = -1; why = "rails warming, holding";
                }
                mudWindows = 0;
            } else if (centerFrac > kCenterStarved && !persistentSilence) {
                // quantization starvation: p50 is pinned at 1 by
                // discreteness, so the floor metrics cannot steer and
                // more gain would only buy digital noise.
                steps = 0; why = "digital floor regime: riding at sat edge";
            } else if (mudStreak >= 2 && mudWindows < 2 && sat < 0.2) {
                steps = +1; why = "floor in the mud";
            } else {
                steps = 0;
                why = (mudStreak < 2) ? "below setpoint (confirming)"
                                      : "digital floor regime: riding at sat edge";
            }
        } else if (rms > 16.0) {
            if (vgaIdx > VGA_LINEAR_IDX && m_vgaSupported) {
                const int candidate = vgaIdx - 1;
                why = "compression headroom: VGA back";
                Log::msg("RtlSdrDevice") << "adaptive gain: VGA -> " << candidate
                          << " (compression headroom)";
                std::lock_guard<std::recursive_mutex> control(m_controlMutex);
                if (applyVgaGain(candidate))
                    vgaIdx = candidate;
                else {
                    steps = -1;
                    why = "compression headroom";
                }
            } else {
                steps = -1;
                why = "compression headroom";
            }
            mudWindows = 0;
        } else {
            mudWindows = 0;          // back in band: re-arm the guard
            mudStreak = 0;
            hotStreak = 0;
            // Inside the band is not the same as where the band wanted us.
            // Close on the target while a step still lands nearer to it,
            // with the two-window confirmation the edges already use.
            if (sigmaFloor < kSigmaTarget && sat < 0.2 && !persistentSilence &&
                climbClosesOnTarget(sigmaFloor, kSigmaTarget, kSigmaHigh,
                                    stepResponseDb)) {
                if (++lowStreak >= 2) {
                    steps = +1;
                    targetClimb = true;
                    why = "closing on setpoint";
                    lowStreak = 0;
                } else {
                    why = "in band, below setpoint (confirming)";
                }
            } else {
                lowStreak = 0;
            }
        }
        // hard-overload double window bookkeeping
        prevP50 = p50;

        Log::info("RtlSdrDevice") << "adaptive gain: sat=" << sat << "% rms="
                  << rms << " p50=" << p50 << " sigma_floor=" << sigmaFloor
                  << " -> " << why;
        reportDiagnostics(why);

        if (steps == 0)
            continue;

        // the tuner gain steps are discrete; ask 2 dB per step and let
        // nearestGain() snap to the table
        int current;
        int gains[256];
        int gainCount;
        {
            std::lock_guard<std::recursive_mutex> control(m_controlMutex);
            current = static_cast<int>(std::lround(m_state.gain_db * 10.0f));
            gainCount = rtlsdr_get_tuner_gains(m_dev, gains);
        }
        int currentIndex = -1;
        for (int i = 0; i < gainCount; ++i)
            if (gains[i] == current) { currentIndex = i; break; }
        int targetIndex = currentIndex;
        if (currentIndex >= 0)
            targetIndex = adaptiveGainTargetIndex(currentIndex, gainCount, steps);
        const int snapped = (targetIndex >= 0) ? gains[targetIndex] : current;
        if (snapped != current) {
            Log::msg("RtlSdrDevice") << "adaptive gain: " << current / 10.0f
                      << " -> " << snapped / 10.0f << " dB (" << why << ")";
            std::lock_guard<std::recursive_mutex> control(m_controlMutex);
            if (applyManualTunerGain(
                    [&] { return rtlsdr_set_tuner_gain_mode(m_dev, 1); },
                    [&] { return rtlsdr_set_tuner_gain(m_dev, snapped); })) {
                recordLinearGain(snapped);
                vgaIdx = VGA_LINEAR_IDX;
            } else {
                continue;
            }
            if (steps > 0) {
                climbRms = rms;      // score this climb next window
                awaitingClimb = true;
                pendingClimbFloor = sigmaFloor;
            }
        } else if (steps > 0) {
            // the table has no higher step: the combined gain is at the
            // mechanical top. In the digital-floor regime the climb does
            // not end here - the VGA is the remaining knob, and the
            // measured response is flat up to +12 dB above the top
            // (saturation still ~0%). Climb it while the ADC stays quiet;
            // the 'rails warming' branch walks it back when they warm.
            if ((sigmaFloor < kSigmaLow || targetClimb) && gainCount > 0 &&
                currentIndex == gainCount - 1 &&
                m_vgaSupported && vgaIdx < 15 && sat < 0.2) {
                const int prev = vgaIdx;
                vgaIdx++;
                Log::msg("RtlSdrDevice") << "adaptive gain: VGA " << prev
                          << " -> " << vgaIdx << " (digital climb, sat="
                          << sat << "%)";
                std::lock_guard<std::recursive_mutex> control(m_controlMutex);
                if (!applyVgaGain(vgaIdx))
                    vgaIdx = m_state.vga_gain;
                else
                    pendingClimbFloor = sigmaFloor;
            } else {
                // the setpoint is unreachable by definition, so stop
                // asking for it
                mudWindows = 2;
            }
        }
    }
}
