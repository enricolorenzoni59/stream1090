/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "devices/SdrplayDevice.hpp"
#include "devices/SdrplayModel.hpp"
#include "Logger.hpp"
#include "Metrics.hpp"
#include <cmath>

bool SdrplayDevice::check(sdrplay_api_ErrT error, const char* operation) const {
    if (error == sdrplay_api_Success) return true;
    Log::error("SDRplay") << operation << ": " << sdrplay_api_GetErrorString(error);
    return false;
}

bool SdrplayDevice::open() {
    if (apiOpen_) return false;
    config_.sdrplay.validate(getSampleRate(), config_.frequencyHz);
    if (!check(sdrplay_api_Open(), "Open (is sdrplay_apiService running?)")) return false;
    apiOpen_ = true;
    if (!check(sdrplay_api_ApiVersion(&apiVersion_), "ApiVersion") ||
        std::fabs(apiVersion_ - SDRPLAY_API_VERSION) > 0.005f) {
        Log::error("SDRplay") << "API runtime/header mismatch: " << apiVersion_ << " / " << SDRPLAY_API_VERSION;
        release(); return false;
    }
    if (!check(sdrplay_api_LockDeviceApi(), "LockDeviceApi")) { release(); return false; }
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES]{};
    unsigned count = 0;
    if (check(sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES), "GetDevices")) {
        for (unsigned n = 0; n < count; ++n) {
            if (!SdrplayModel::available(devices[n])) continue;
            if (config_.serial && *config_.serial != devices[n].SerNo) continue;
            const auto model = SdrplayModel::lookup(devices[n].hwVer);
            const bool duo = devices[n].hwVer == SDRPLAY_RSPduo_ID;
            const bool dx = devices[n].hwVer == SDRPLAY_RSPdx_ID;
            if (config_.sdrplay.lnaState > model.maxLnaState) {
                Log::error("SDRplay") << model.name << " LNA state must be 0.." << model.maxLnaState << " at 1090 MHz";
                continue;
            }
            if ((config_.sdrplay.tuner && !duo) || (!config_.sdrplay.antenna.empty() && !dx)) {
                Log::error("SDRplay", "--sdrplay-tuner requires RSPduo; --sdrplay-antenna requires RSPdx");
                continue;
            }
            if (config_.biasTee && ((duo && config_.sdrplay.tuner != 2) ||
                                   (dx && config_.sdrplay.antenna != "B"))) {
                Log::error("SDRplay", "Bias-T requires RSPduo tuner 2 or RSPdx antenna B");
                continue;
            }
            device_ = devices[n];
            if (duo) {
                const auto tuner = config_.sdrplay.tuner == 2 ? sdrplay_api_Tuner_B : sdrplay_api_Tuner_A;
                if (!(device_.tuner & tuner)) continue;
                device_.tuner = tuner;
                device_.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
                device_.rspDuoSampleFreq = 0; // Only used in dual/master/slave modes.
            }
            if (check(sdrplay_api_SelectDevice(&device_), "SelectDevice")) { selected_ = true; break; }
        }
    }
    const bool unlocked = check(sdrplay_api_UnlockDeviceApi(), "UnlockDeviceApi");
    if (!selected_ || !unlocked) {
        Log::error("SDRplay", "No available RSP1B/RSPduo (single tuner)/RSPdx matching serial and settings");
        release(); return false;
    }
    if (!check(sdrplay_api_GetDeviceParams(device_.dev, &params_), "GetDeviceParams") ||
        !params_ || !params_->devParams) { release(); return false; }
    auto* channel = device_.tuner == sdrplay_api_Tuner_B ? params_->rxChannelB : params_->rxChannelA;
    if (!channel) { Log::error("SDRplay", "Selected tuner has no parameter block"); release(); return false; }
    auto& dev = *params_->devParams;
    auto& rx = *channel;
    dev.fsFreq.fsHz = getSampleRate();
    dev.mode = config_.sdrplay.usbBulk ? sdrplay_api_BULK : sdrplay_api_ISOCH;
    dev.ppm = config_.ppm.value_or(0);
    switch (device_.hwVer) {
    case SDRPLAY_RSP1B_ID:
        dev.rsp1aParams.rfNotchEnable = config_.sdrplay.rfNotch;
        dev.rsp1aParams.rfDabNotchEnable = config_.sdrplay.dabNotch;
        rx.rsp1aTunerParams.biasTEnable = config_.biasTee;
        break;
    case SDRPLAY_RSPduo_ID:
        rx.rspDuoTunerParams.rfNotchEnable = config_.sdrplay.rfNotch;
        rx.rspDuoTunerParams.rfDabNotchEnable = config_.sdrplay.dabNotch;
        rx.rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2; // 50 ohm
        rx.rspDuoTunerParams.tuner1AmNotchEnable = 0;
        rx.rspDuoTunerParams.biasTEnable = config_.biasTee;
        break;
    case SDRPLAY_RSPdx_ID:
        dev.rspDxParams.rfNotchEnable = config_.sdrplay.rfNotch;
        dev.rspDxParams.rfDabNotchEnable = config_.sdrplay.dabNotch;
        dev.rspDxParams.biasTEnable = config_.biasTee;
        dev.rspDxParams.hdrEnable = 0;
        dev.rspDxParams.antennaSel = config_.sdrplay.antenna == "B" ?
            sdrplay_api_RspDx_ANTENNA_B : sdrplay_api_RspDx_ANTENNA_A;
        break;
    }
    rx.tunerParams.rfFreq.rfHz = config_.frequencyHz;
    rx.tunerParams.bwType = static_cast<sdrplay_api_Bw_MHzT>(config_.sdrplay.bandwidthKhz);
    rx.tunerParams.ifType = sdrplay_api_IF_Zero;
    rx.tunerParams.gain.gRdB = config_.sdrplay.gainReduction;
    rx.tunerParams.gain.LNAstate = config_.sdrplay.lnaState;
    rx.ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    rx.ctrlParams.decimation.enable = 0;
    rx.ctrlParams.dcOffset.DCenable = 1;
    rx.ctrlParams.dcOffset.IQenable = 1;
    rx.ctrlParams.adsbMode = static_cast<sdrplay_api_AdsbModeT>(config_.sdrplay.adsbMode);
    Log::msg("SDRplay") << SdrplayModel::lookup(device_.hwVer).name << " " << device_.SerNo
        << ", tuner " << (device_.tuner == sdrplay_api_Tuner_B ? 2 : 1)
        << ", antenna " << (device_.hwVer == SDRPLAY_RSPdx_ID ?
            (config_.sdrplay.antenna == "B" ? "B" : "A") : "50ohm") << ", API " << apiVersion_
        << ", " << getSampleRate() << " complex samples/s, nominal ADC "
        << SdrplaySettings::nominalAdcBits(getSampleRate()) << " bits, API signed-16, IF GR "
        << config_.sdrplay.gainReduction << " dB, LNA state " << config_.sdrplay.lnaState
        << ", USB " << (config_.sdrplay.usbBulk ? "bulk" : "isoch");
    return true;
}

bool SdrplayDevice::start() {
    if (!selected_ || initialized_) return false;
    haveSequence_ = false;
    iqStats_.reset();
    invalidateTelemetry();
    if constexpr (Metrics::Enabled) {
        Metrics::registry().sdrplayIqLastSteady.set(0);
        Metrics::registry().sdrplayOverloadActive.set(-1);
    }
    sdrplay_api_CallbackFnsT callbacks{};
    // API single-tuner mode delivers the selected tuner through stream A,
    // including RSPduo tuner 2 (see the vendor API example).
    callbacks.StreamACbFn = streamCallback;
    callbacks.StreamBCbFn = [](short*, short*, sdrplay_api_StreamCbParamsT*, unsigned, unsigned, void* ctx) noexcept {
        auto& self = *static_cast<SdrplayDevice*>(ctx);
        if (self.m_running.load()) self.fail("unexpected_second_stream");
    };
    callbacks.EventCbFn = eventCallback;
    m_running.store(true);
    if (!check(sdrplay_api_Init(device_.dev, &callbacks, this), "Init")) {
        m_running.store(false); return false;
    }
    initialized_ = true;
    if constexpr (Metrics::Enabled) {
        auto& reg = Metrics::registry();
        reg.sdrplaySampleRate.set(getSampleRate());
        reg.sdrplayAdcBits.set(SdrplaySettings::nominalAdcBits(getSampleRate()));
        reg.sdrplayIfGr.set(config_.sdrplay.gainReduction);
        reg.sdrplayLnaState.set(config_.sdrplay.lnaState);
    }
    return !failed_.load();
}

void SdrplayDevice::streamCallback(short* i, short* q, sdrplay_api_StreamCbParamsT* p,
                                  unsigned count, unsigned reset, void* context) noexcept {
    auto& self = *static_cast<SdrplayDevice*>(context);
    try {
        if (!self.m_running.load()) return;
        self.callbacks_.fetch_add(1);
        if constexpr (Metrics::Enabled) Metrics::registry().sdrplayCallbacks.inc();
        self.markAsAlive();
        if (reset) {
            self.resets_.fetch_add(1);
            if constexpr (Metrics::Enabled) Metrics::registry().sdrplayResets.inc();
            // Startup may announce reset; a later reset terminates this epoch.
            if (self.haveSequence_) { self.fail("stream_reset"); return; }
        }
        if (!count) return;
        if (!i || !q || !p) { self.fail("invalid_callback"); return; }
        if (self.haveSequence_ && p->firstSampleNum != self.nextSample_) {
            self.iqStats_.reset();
            self.invalidateTelemetry();
            self.gaps_.fetch_add(1);
            if constexpr (Metrics::Enabled) Metrics::registry().sdrplayGaps.inc();
            const uint32_t missing = p->firstSampleNum - self.nextSample_;
            self.missing_.fetch_add(missing);
            if constexpr (Metrics::Enabled) {
                auto& reg = Metrics::registry();
                if (missing < 0x80000000u) {
                    const double seconds = double(missing) / double(self.getSampleRate());
                    reg.sdrplayMissingSamples.inc(missing);
                    reg.sdrplayGapDuration.observe(seconds, Metrics::SdrplayGapBounds);
                    // One selected stream owns these updates; registry survives recovery.
                    reg.sdrplayLargestGap.set(std::max(reg.sdrplayLargestGap.get(), seconds));
                } else reg.sdrplaySequenceErrors.inc();
            }
            // A forward modular delta below half the counter range retains
            // the sample clock. Backward/ambiguous jumps require recovery.
            // Raw capture writers deliberately decline segmentation.
            if (missing >= 0x80000000u || !self.m_bufferWriter.discontinuity(uint64_t(missing) * 2)) {
                self.fail("sample_gap"); return;
            }
        }
        self.haveSequence_ = true;
        self.nextSample_ = uint32_t(p->firstSampleNum + count);
        if constexpr (Metrics::Enabled) {
            if (Metrics::registry().signalQualityCollection())
                self.iqStats_.observe(i, q, count, self.getSampleRate(),
                    [&self](const SdrplayIqStats::Window& window) { self.publishTelemetry(window); });
        }
        for (size_t base = 0; base < count && self.m_running.load(); base += 4096) {
            const size_t n = std::min<size_t>(4096, count - base);
            for (size_t k = 0; k < n; ++k) {
                self.interleaved_[2*k] = i[base+k];
                self.interleaved_[2*k+1] = q[base+k];
            }
            self.writeDataToBuffer(self.interleaved_.data(), 2*n);
        }
    } catch (...) { self.fail("stream_callback_exception"); } // Never unwind through the vendor's C callback.
}

void SdrplayDevice::eventCallback(sdrplay_api_EventT event, sdrplay_api_TunerSelectT tuner,
                                 sdrplay_api_EventParamsT* params, void* context) noexcept {
    auto& self = *static_cast<SdrplayDevice*>(context);
    try {
        // Ignore terminal events emitted by deliberate Uninit/Release.
        if (!self.m_running.load()) return;
        if (event == sdrplay_api_PowerOverloadChange) {
            if (params && params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Detected) {
                self.overloads_.fetch_add(1);
                if constexpr (Metrics::Enabled) Metrics::registry().sdrplayOverloads.inc();
            }
            if constexpr (Metrics::Enabled) {
                if (params) {
                    const auto change = params->powerOverloadParams.powerOverloadChangeType;
                    if (change == sdrplay_api_Overload_Detected) Metrics::registry().sdrplayOverloadActive.set(1);
                    else if (change == sdrplay_api_Overload_Corrected) Metrics::registry().sdrplayOverloadActive.set(0);
                }
            }
            if (sdrplay_api_Update(self.device_.dev, tuner, sdrplay_api_Update_Ctrl_OverloadMsgAck,
                                   sdrplay_api_Update_Ext1_None) != sdrplay_api_Success) self.fail("overload_ack_failed");
        } else if (event == sdrplay_api_DeviceRemoved) self.fail("device_removed");
        else if (event == sdrplay_api_DeviceFailure) self.fail("device_failure");
    } catch (...) { self.fail("event_callback_exception"); }
}

void SdrplayDevice::stop() {
    m_running.store(false);
    shutdownWriter(); // Unblock an in-flight callback before Uninit waits for it.
    if (initialized_) {
        check(sdrplay_api_Uninit(device_.dev), "Uninit");
        initialized_ = false;
    }
    invalidateTelemetry();
    if constexpr (Metrics::Enabled) Metrics::registry().sdrplayOverloadActive.set(-1);
}

void SdrplayDevice::invalidateTelemetry() noexcept {
    if constexpr (Metrics::Enabled) Metrics::registry().sdrplayIqValid.set(0);
}

void SdrplayDevice::publishTelemetry(const SdrplayIqStats::Window& w) noexcept {
    if constexpr (Metrics::Enabled) {
        auto& reg = Metrics::registry();
        reg.sdrplayIqValid.set(0);
        reg.sdrplayIqScalars.set(w.scalars);
        reg.sdrplayIqRms.set(w.rmsDbfs);
        reg.sdrplayIqMedian.set(w.medianDbfs);
        reg.sdrplayIqP999.set(w.p999Dbfs);
        reg.sdrplayIqPeak.set(w.peakDbfs);
        reg.sdrplayIqNoiseSigma.set(w.noiseSigmaDbfs);
        reg.sdrplayIqRails.set(w.railFraction);
        reg.sdrplayIqNearFull.set(w.nearFullFraction);
        reg.sdrplayIqCenter.set(w.centerFraction);
        reg.sdrplayIqWindows.inc();
        reg.sdrplayIqLastSteady.set(Metrics::Registry::steadySeconds());
        reg.sdrplayIqValid.set(m_running.load() ? 1 : 0);
    }
}

void SdrplayDevice::release() {
    if (selected_) {
        if (check(sdrplay_api_LockDeviceApi(), "LockDeviceApi (release)")) {
            check(sdrplay_api_ReleaseDevice(&device_), "ReleaseDevice");
            check(sdrplay_api_UnlockDeviceApi(), "UnlockDeviceApi (release)");
        }
        selected_ = false;
    }
    if (apiOpen_) { check(sdrplay_api_Close(), "Close"); apiOpen_ = false; }
    params_ = nullptr;
}

void SdrplayDevice::close() {
    const bool wasOpen = apiOpen_;
    stop(); release();
    if (wasOpen && failed_.load()) Log::error("SDRplay") << "Session failed: " << failureReason_.load();
}

std::map<std::string, std::string> SdrplayDevice::captureMetadata() const {
    return {{"device", SdrplayModel::lookup(device_.hwVer).name ? SdrplayModel::lookup(device_.hwVer).name : "unknown"}, {"serial", device_.SerNo}, {"api_version", std::to_string(apiVersion_)},
        {"tuner", device_.tuner == sdrplay_api_Tuner_B ? "2" : "1"},
        {"antenna", device_.hwVer == SDRPLAY_RSPdx_ID ? (config_.sdrplay.antenna == "B" ? "B" : "A") : "50ohm"},
        {"duo_mode", device_.hwVer == SDRPLAY_RSPduo_ID ? "single_tuner" : "not_applicable"},
        {"nominal_adc_bits", std::to_string(SdrplaySettings::nominalAdcBits(getSampleRate()))},
        {"if_gain_reduction_db", std::to_string(config_.sdrplay.gainReduction)},
        {"lna_state", std::to_string(config_.sdrplay.lnaState)},
        {"bandwidth_khz", std::to_string(config_.sdrplay.bandwidthKhz)},
        {"adsb_mode", std::to_string(config_.sdrplay.adsbMode)},
        {"usb_transfer_mode", config_.sdrplay.usbBulk ? "bulk" : "isoch"},
        {"bias_tee", config_.biasTee ? "true" : "false"}, {"ppm", std::to_string(config_.ppm.value_or(0))},
        {"rf_notch", config_.sdrplay.rfNotch ? "true" : "false"},
        {"dab_notch", config_.sdrplay.dabNotch ? "true" : "false"},
        {"api_dc_correction", "true"}, {"api_iq_correction", "true"}, {"decimation", "false"},
        {"callbacks", std::to_string(callbacks_.load())}, {"resets", std::to_string(resets_.load())},
        {"gap_events", std::to_string(gaps_.load())}, {"missing_samples_modulo_2_32", std::to_string(missing_.load())},
        {"overload_events", std::to_string(overloads_.load())}, {"stream_failed", failed_.load() ? "true" : "false"}, {"failure_reason", failureReason_.load()}};
}
