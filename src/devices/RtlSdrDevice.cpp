/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#include "devices/RtlSdrDevice.hpp"
#include "AdaptiveGainLogic.hpp"
#include "AdcHistogramDiagnostics.hpp"
#include "devices/RtlSdrSerial.hpp"
#include "Counters.hpp"
#include "Logger.hpp"
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <thread>
#include <sstream>

static void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    auto* self = static_cast<RtlSdrDevice*>(ctx);

    if (!self->isRunning())
        return;

    self->markAsAlive();
    // The histogram feeds the adaptive loop and periodic diagnostics.
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
        Log::error("RtlSdrDevice") << "No RTL-SDR device found with serial '"
                  << serial << "'";
        return false;
    }

    if (rtlsdr_open(&m_dev, index) != 0)
        return false;

    // identify the tuner on every successful open: bug reports for RTL-SDR
    // issues are ambiguous without it, and the r82xx behaviour in
    // particular is what the tuner_bandwidth documentation is about.
    const char* tuner_name = "?";
    switch (rtlsdr_get_tuner_type(m_dev)) {
        case RTLSDR_TUNER_R820T:  tuner_name = "R820T/R820T2"; break;
        case RTLSDR_TUNER_R828D:  tuner_name = "R828D"; break;
        case RTLSDR_TUNER_E4000:  tuner_name = "E4000"; break;
        case RTLSDR_TUNER_FC0012: tuner_name = "FC0012"; break;
        case RTLSDR_TUNER_FC0013: tuner_name = "FC0013"; break;
        case RTLSDR_TUNER_FC2580: tuner_name = "FC2580"; break;
        default: break;
    }
    std::cerr << "[RtlSdrDevice] Tuner: " << tuner_name << std::endl;

    auto check = [&](const char* name, int rc) {
        if (rc != 0) {
            Log::error("RtlSdrDevice") << "ERROR: " << name
                    << " failed with code " << rc;
            return false;
        }
        return true;
    };

    // Set the frequency before the sample rate: R820T bandwidth setup retunes
    // the current frequency, and immediately after open() that value is zero.
    if (!check("rtlsdr_set_center_freq",
            rtlsdr_set_center_freq(m_dev, 1090000000)))
        return false;

    if (!check("rtlsdr_set_sample_rate",
            rtlsdr_set_sample_rate(m_dev, getSampleRate())))
        return false;

    if (!check("rtlsdr_reset_buffer",
            rtlsdr_reset_buffer(m_dev)))
        return false;
    return true;
}

bool RtlSdrDevice::open_with_serial(uint64_t serial) {
    int deviceCount = rtlsdr_get_device_count();
    if (deviceCount <= 0)
        return false;

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

    if (rtlsdr_open(&m_dev, index) != 0)
        return false;

    // identify the tuner on every successful open: bug reports for RTL-SDR
    // issues are ambiguous without it, and the r82xx behaviour in
    // particular is what the tuner_bandwidth documentation is about.
    const char* tuner_name = "?";
    switch (rtlsdr_get_tuner_type(m_dev)) {
        case RTLSDR_TUNER_R820T:  tuner_name = "R820T/R820T2"; break;
        case RTLSDR_TUNER_R828D:  tuner_name = "R828D"; break;
        case RTLSDR_TUNER_E4000:  tuner_name = "E4000"; break;
        case RTLSDR_TUNER_FC0012: tuner_name = "FC0012"; break;
        case RTLSDR_TUNER_FC0013: tuner_name = "FC0013"; break;
        case RTLSDR_TUNER_FC2580: tuner_name = "FC2580"; break;
        default: break;
    }
    std::cerr << "[RtlSdrDevice] Tuner: " << tuner_name << std::endl;
    
    
    auto check = [&](const char* name, int rc) {
        if (rc != 0) {
            Log::error("RtlSdrDevice") << "ERROR: " << name
                    << " failed with code " << rc;
            return false;
        }
        return true;
    };

    // Set the frequency before the sample rate: R820T bandwidth setup retunes
    // the current frequency, and immediately after open() that value is zero.
    if (!check("rtlsdr_set_center_freq",
            rtlsdr_set_center_freq(m_dev, 1090000000)))
        return false;

    if (!check("rtlsdr_set_sample_rate",
            rtlsdr_set_sample_rate(m_dev, getSampleRate())))
        return false;

    if (!check("rtlsdr_reset_buffer",
            rtlsdr_reset_buffer(m_dev)))
        return false;
    return true;
}

bool RtlSdrDevice::open() {
    if (!open_with_serial(m_serialString))
        return false;

    const char* tunerName = "unknown";
    switch (rtlsdr_get_tuner_type(m_dev)) {
        case RTLSDR_TUNER_R820T:  tunerName = "R820T/R820T2"; break;
        case RTLSDR_TUNER_R828D:  tunerName = "R828D"; break;
        case RTLSDR_TUNER_E4000:  tunerName = "E4000"; break;
        case RTLSDR_TUNER_FC0012: tunerName = "FC0012"; break;
        case RTLSDR_TUNER_FC0013: tunerName = "FC0013"; break;
        case RTLSDR_TUNER_FC2580: tunerName = "FC2580"; break;
        default: break;
    }
#ifdef STREAM1090_HAVE_RTLSDR_BLOG
    const int tuner = rtlsdr_get_tuner_type(m_dev);
    m_vgaSupported = tuner == RTLSDR_TUNER_R820T || tuner == RTLSDR_TUNER_R828D;
#else
    m_vgaSupported = false;
#endif
    std::cerr << "[RtlSdrDevice] Tuner: " << tunerName << std::endl;
    return true;
}

// ----------------------
// Start / Stop / Close
// ----------------------
bool RtlSdrDevice::start() {
    if (!m_dev || !m_gainControlReady) {
        Log::error("RtlSdrDevice") << "RTL-SDR adaptive gain was not initialized";
        return false;
    }

    m_running.store(true, std::memory_order_relaxed);

    startAdaptiveGain();

    m_thread = std::thread([this]() {
        int rc = rtlsdr_read_async(
            m_dev,
            rtlsdr_callback,
            this,
            0,
            0
        );

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

    stopAdaptiveGain();
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
    m_agcApplied = false;
    m_gainControlReady = false;
    m_initialConfigApplied = false;
    m_state.gain_known = false;
}

// r82xx per-step gains in tenths of dB, from the vendored tuner_r82xx.c:
// the "linear" gain walks LNA and MIXER alternately until the requested
// total, with the VGA fixed at index 8 (16.3 dB)
static constexpr int kLnaSteps[] = {0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13};
static constexpr int kMixSteps[] = {0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8};

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

bool RtlSdrDevice::vgaSupported() const {
    return m_vgaSupported;
}

bool RtlSdrDevice::applyVgaGain(int value) {
#ifdef STREAM1090_HAVE_RTLSDR_BLOG
    if (!vgaSupported() || value < 0 || value > 15)
        return false;
    if (rtlsdr_r82xx_set_vga_gain(m_dev, value) != 0)
        return false;
    m_state.vga_gain = value;
    m_state.vga_known = true;
    return true;
#else
    (void)value;
    return false;
#endif
}


// ----------------------
// Shadow-aware setters with change logging
// ----------------------

bool RtlSdrDevice::setFrequency(uint32_t hz) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (m_state.frequency == hz)
        return true;

    if (rtlsdr_set_center_freq(m_dev, hz) == 0) {
        Log::info("RtlSdrDevice") << "frequency: "
                  << m_state.frequency << " -> " << hz;
        m_state.frequency = hz;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setGain(float gainDb) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (!shadowValueNeedsApply(m_state.gain_known, m_state.gain_db, gainDb))
        return true;

    int gainTenths = static_cast<int>(gainDb * 10.0f);
    int nearest = nearestGain(gainTenths);

    if (applyManualTunerGain(
            [&] { return rtlsdr_set_tuner_gain_mode(m_dev, 1); },
            [&] { return rtlsdr_set_tuner_gain(m_dev, nearest); })) {
        Log::info("RtlSdrDevice") << "gain: "
                  << m_state.gain_db << " dB -> " << gainDb << " dB"
                  << " (nearest step = " << nearest/10.0f << " dB)";
        m_state.gain_db = nearest / 10.0f;
        m_state.gain_known = true;
        // track the partition the linear walk chose (for the exporter)
        int lnaIdx = 0, mixIdx = 0;
        r82xxWalkPartition(nearest, lnaIdx, mixIdx);
        m_state.lna_gain = lnaIdx;
        m_state.mixer_gain = mixIdx;
        m_state.vga_gain = 8; // the linear path fixes the VGA at 16.3 dB
        m_state.lna_known = m_vgaSupported;
        m_state.mixer_known = m_vgaSupported;
        m_state.vga_known = m_vgaSupported;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setAgc(bool enabled) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (m_agcApplied && m_state.agc == enabled)
        return true;

    if (rtlsdr_set_agc_mode(m_dev, enabled ? 1 : 0) == 0) {
        Log::info("RtlSdrDevice") << "agc: "
                  << (m_state.agc ? "on" : "off")
                  << " -> " << (enabled ? "on" : "off");
        if (enabled) {
            Log::warn("RtlSdrDevice")
                << "agc=true enables the RTL2832U digital AGC, not the tuner AGC. "
                   "It lifts the noise floor between pulses and usually lowers the "
                   "Mode-S message rate; prefer agc=false with an explicit gain.";
        }
        m_state.agc = enabled;
        m_agcApplied = true;
        return true;
    }
    return false;
}

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
    Log::info("RtlSdrDevice") << "adaptive gain: start from " << m_state.gain_db << " dB";
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
    // the VGA index the linear path fixes (16.3 dB): the digital-floor
    // climb may push it above this, the rebalance paths walk it back
    constexpr int VGA_LINEAR_IDX = 8;

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
    int vgaIdx = m_state.vga_gain;

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
                std::lock_guard<std::mutex> control(m_controlMutex);
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
            Log::info("RtlSdrDiagnostics") << line.str();

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
        // nothing: prevP50 is deliberately not updated either.
        if (rms < kSilentRms && sat <= 0.0) {
            Log::info("RtlSdrDevice") << "adaptive gain: input dropout (rms="
                      << rms << " sat=" << sat << "%), window skipped";
            reportDiagnostics("input dropout, holding");
            continue;
        }

        // Windup guard, first half: score the previous climb. Two
        // non-responding climbs in a row declare the setpoint
        // unreachable and the loop holds until another decision
        // re-arms it.
        if (awaitingClimb) {
            const double responseDb = (climbRms > 0.0 && rms > 0.0)
                ? 20.0 * std::log10(rms / climbRms) : 0.0;
            if (responseDb < kClimbResponseDb)
                mudWindows++;
            else
                mudWindows = 0;
            awaitingClimb = false;
        }

        int steps = 0;
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
                    Log::info("RtlSdrDevice") << "adaptive gain: VGA -> " << candidate
                              << " (floor too hot)";
                    std::lock_guard<std::mutex> control(m_controlMutex);
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
                if (vgaIdx > VGA_LINEAR_IDX && vgaSupported()) {
                    // the headroom was bought with VGA: give it back
                    // before touching the combined gain
                    const int candidate = vgaIdx - 1;
                    std::lock_guard<std::mutex> control(m_controlMutex);
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
            } else if (centerFrac > kCenterStarved) {
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
            if (vgaIdx > VGA_LINEAR_IDX && vgaSupported()) {
                const int candidate = vgaIdx - 1;
                why = "compression headroom: VGA back";
                Log::info("RtlSdrDevice") << "adaptive gain: VGA -> " << candidate
                          << " (compression headroom)";
                std::lock_guard<std::mutex> control(m_controlMutex);
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
        {
            std::lock_guard<std::mutex> control(m_controlMutex);
            current = static_cast<int>(std::lround(m_state.gain_db * 10.0f));
        }
        int gains[256];
        const int gainCount = rtlsdr_get_tuner_gains(m_dev, gains);
        int currentIndex = -1;
        for (int i = 0; i < gainCount; ++i)
            if (gains[i] == current) { currentIndex = i; break; }
        int targetIndex = currentIndex;
        if (currentIndex >= 0)
            targetIndex = adaptiveGainTargetIndex(currentIndex, gainCount, steps);
        const int snapped = (targetIndex >= 0) ? gains[targetIndex] : current;
        if (snapped != current) {
            Log::info("RtlSdrDevice") << "adaptive gain: " << current / 10.0f
                      << " -> " << snapped / 10.0f << " dB (" << why << ")";
            std::lock_guard<std::mutex> control(m_controlMutex);
            if (applyManualTunerGain(
                    [&] { return rtlsdr_set_tuner_gain_mode(m_dev, 1); },
                    [&] { return rtlsdr_set_tuner_gain(m_dev, snapped); })) {
                m_state.gain_db = snapped / 10.0f;
                m_state.gain_known = true;
                int lnaIdx = 0, mixIdx = 0;
                r82xxWalkPartition(snapped, lnaIdx, mixIdx);
                m_state.lna_gain = lnaIdx;
                m_state.mixer_gain = mixIdx;
                m_state.vga_gain = VGA_LINEAR_IDX;
                m_state.lna_known = m_vgaSupported;
                m_state.mixer_known = m_vgaSupported;
                m_state.vga_known = m_vgaSupported;
                vgaIdx = VGA_LINEAR_IDX;
            } else {
                continue;
            }
            if (steps > 0) {
                climbRms = rms;      // score this climb next window
                awaitingClimb = true;
            }
        } else if (steps > 0) {
            // the table has no higher step: the combined gain is at the
            // mechanical top. In the digital-floor regime the climb does
            // not end here - the VGA is the remaining knob, and the
            // measured response is flat up to +12 dB above the top
            // (saturation still ~0%). Climb it while the ADC stays quiet;
            // the 'rails warming' branch walks it back when they warm.
            if (sigmaFloor < kSigmaLow && gainCount > 0 && currentIndex == gainCount - 1 &&
                vgaSupported() && vgaIdx < 15 && sat < 0.2) {
                const int prev = vgaIdx;
                vgaIdx++;
                Log::info("RtlSdrDevice") << "adaptive gain: VGA " << prev
                          << " -> " << vgaIdx << " (digital climb, sat="
                          << sat << "%)";
                std::lock_guard<std::mutex> control(m_controlMutex);
                if (!applyVgaGain(vgaIdx))
                    vgaIdx = m_state.vga_gain;
            } else {
                // the setpoint is unreachable by definition, so stop
                // asking for it
                mudWindows = 2;
            }
        }
    }
}

bool RtlSdrDevice::setBiasTee(bool enabled) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (m_state.bias_tee == enabled)
        return true;

    if (rtlsdr_set_bias_tee(m_dev, enabled ? 1 : 0) == 0) {
        Log::info("RtlSdrDevice") << "bias_tee: "
                  << (m_state.bias_tee ? "on" : "off")
                  << " -> " << (enabled ? "on" : "off");
        m_state.bias_tee = enabled;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setPpm(int ppm) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (m_state.ppm == ppm)
        return true;

    if (rtlsdr_set_freq_correction(m_dev, ppm) == 0) {
        Log::info("RtlSdrDevice") << "ppm: "
                  << m_state.ppm << " -> " << ppm;
        m_state.ppm = ppm;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setOffsetTuning(bool enabled) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (m_state.offset_tuning == enabled)
        return true;

    if (rtlsdr_set_offset_tuning(m_dev, enabled ? 1 : 0) == 0) {
        Log::info("RtlSdrDevice") << "offset_tuning: "
                  << (m_state.offset_tuning ? "on" : "off")
                  << " -> " << (enabled ? "on" : "off");
        m_state.offset_tuning = enabled;
        return true;
    }
    return false;
}

bool RtlSdrDevice::setTunerBandwidth(uint32_t bw) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (m_state.tuner_bandwidth == bw)
        return true;

    if (rtlsdr_set_tuner_bandwidth(m_dev, bw) == 0) {
        Log::info("RtlSdrDevice") << "tuner_bandwidth: "
                  << m_state.tuner_bandwidth << " -> " << bw;
        m_state.tuner_bandwidth = bw;
        return true;
    }
    return false;
}

#ifdef STREAM1090_HAVE_RTLSDR_BLOG
bool RtlSdrDevice::setLnaGain(int gain) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (!m_dev)
        return false;

    // Shadow awareness
    if (!shadowValueNeedsApply(m_state.lna_known, m_state.lna_gain, gain))
        return true;

    if (rtlsdr_r82xx_set_lna_gain(m_dev, gain) != 0)
        return false;

    Log::info("RtlSdrDevice") << "LNA gain: "
              << m_state.lna_gain << " -> " << gain;

    m_state.lna_gain = gain;
    m_state.lna_known = true;
    return true;
}

bool RtlSdrDevice::setMixerGain(int gain) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (!m_dev)
        return false;

    // Shadow awareness
    if (!shadowValueNeedsApply(m_state.mixer_known, m_state.mixer_gain, gain))
        return true;

    if (rtlsdr_r82xx_set_mixer_gain(m_dev, gain) != 0)
        return false;

    Log::info("RtlSdrDevice") << "Mixer gain: "
              << m_state.mixer_gain << " -> " << gain;

    m_state.mixer_gain = gain;
    m_state.mixer_known = true;
    return true;
}

bool RtlSdrDevice::setVgaGain(int gain) {
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (!m_dev)
        return false;

    // Shadow awareness
    if (!shadowValueNeedsApply(m_state.vga_known, m_state.vga_gain, gain))
        return true;

    const int old = m_state.vga_gain;
    if (!applyVgaGain(gain))
        return false;
    Log::info("RtlSdrDevice") << "VGA gain: " << old << " -> " << gain;
    return true;
}

#else
bool RtlSdrDevice::setLnaGain(int) { return false; }
bool RtlSdrDevice::setMixerGain(int) { return false; }
bool RtlSdrDevice::setVgaGain(int) { return false; }
#endif





// ----------------------
// applySetting()
// ----------------------
bool RtlSdrDevice::applySetting(const std::string& key, const std::string& value) {
    if (!m_dev)
        return false;

    // Keep old INI files usable, but never let them override this test's gain
    // policy. Ignore even malformed values without parsing them.
    if (key == "gain" || key == "agc" || key == "adaptive_gain" ||
        key == "lna_gain" || key == "mixer_gain" || key == "vga_gain")
        return true;

    // Core controls
    if (key == "frequency")        return setFrequency(std::stoul(value));
    if (key == "bias_tee")         return setBiasTee(value == "1" || value == "true" || value == "on");
    if (key == "ppm")              return setPpm(std::stoi(value));
    if (key == "offset_tuning")    return setOffsetTuning(value == "1" || value == "true" || value == "on");
    if (key == "tuner_bandwidth")  return setTunerBandwidth(std::stoul(value));

    return false;
}


void RtlSdrDevice::applyConfigPreOpen(const IniConfig::Section& cfg) {
    for (auto& [key, value] : cfg) {

        if (key == "serial")
            m_serialString = value;
    }
}

// ----------------------
// Reload logic
// ----------------------
void RtlSdrDevice::applyConfigPostOpen(const IniConfig::Section& cfg) {
    // Apply a reload as one control transaction. The worker must not retain
    // a local decision from the previous configuration while setters run.
    std::lock_guard<std::mutex> configLock(m_configMutex);
    stopAdaptiveGain();
    if (!m_initialConfigApplied) {
        m_initialConfigApplied = true;

        if (!cfg.count("tuner_bandwidth")
                && rtlsdr_get_tuner_type(m_dev) == RTLSDR_TUNER_R820T) {
            Log::warn("RtlSdrDevice")
                << "No tuner_bandwidth configured for this R820T/R820T2 tuner; "
                   "automatic IF filter selection depends on the sample rate and "
                   "librtlsdr implementation. Set it explicitly (for example, "
                   "3000000 at 2.4 or 2.56 Msps) to make the tuner state reproducible.";
        }

    }

    // A reproducible starting point: disable the RTL2832U digital AGC and
    // request manual tuner gain before the adaptive worker starts. Reloads
    // preserve the gain found by the controller.
    if (!m_gainControlReady) {
        m_gainControlReady = setAgc(false) && setGain(49.6f);
        if (!m_gainControlReady)
            Log::error("RtlSdrDevice") << "Cannot initialize forced adaptive gain";
    }

    for (auto& [key, value] : cfg) {

        if (key == "serial")
            continue; // immutable

        try {
            if (!applySetting(key, value)) {
                Log::warn("RtlSdrDevice") << "Setting not applied: " << key;
            }
        } catch (const std::exception& e) {
            Log::error("RtlSdrDevice") << "Invalid setting " << key << ": " << e.what();
        }
    }

    if (m_running.load(std::memory_order_relaxed) && m_gainControlReady)
        startAdaptiveGain();

    // Report the bandwidth setting once. librtlsdr has no read-back API for
    // the effective bandwidth it derives when tuner_bandwidth is omitted.
    if (!m_stateReported) {
        m_stateReported = true;
        ShadowState state;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            state = m_state;
        }
        std::cerr << "[RtlSdrDevice] Tuner bandwidth setting: "
                  << (state.tuner_bandwidth
                          ? std::to_string(state.tuner_bandwidth) + " Hz (explicit)"
                          : std::string("auto (derived by librtlsdr from sample rate)"))
                  << std::endl;
    }
}
