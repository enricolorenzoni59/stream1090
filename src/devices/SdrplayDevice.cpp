/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "devices/SdrplayDevice.hpp"
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
            if (devices[n].hwVer != SDRPLAY_RSP1B_ID || !devices[n].valid) continue;
            if (config_.serial && *config_.serial != devices[n].SerNo) continue;
            device_ = devices[n];
            if (check(sdrplay_api_SelectDevice(&device_), "SelectDevice")) { selected_ = true; break; }
        }
    }
    const bool unlocked = check(sdrplay_api_UnlockDeviceApi(), "UnlockDeviceApi");
    if (!selected_ || !unlocked) {
        Log::error("SDRplay", "No selectable RSP1B matching the requested serial");
        release(); return false;
    }
    if (!check(sdrplay_api_GetDeviceParams(device_.dev, &params_), "GetDeviceParams") ||
        !params_ || !params_->devParams || !params_->rxChannelA) { release(); return false; }
    auto& dev = *params_->devParams;
    auto& rx = *params_->rxChannelA;
    dev.fsFreq.fsHz = getSampleRate();
    dev.mode = config_.sdrplay.usbBulk ? sdrplay_api_BULK : sdrplay_api_ISOCH;
    dev.ppm = config_.ppm.value_or(0);
    dev.rsp1aParams.rfNotchEnable = config_.sdrplay.rfNotch;
    dev.rsp1aParams.rfDabNotchEnable = config_.sdrplay.dabNotch;
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
    rx.rsp1aTunerParams.biasTEnable = config_.biasTee;
    Log::msg("SDRplay") << "RSP1B " << device_.SerNo << ", API " << apiVersion_
        << ", " << getSampleRate() << " complex samples/s, nominal ADC "
        << SdrplaySettings::nominalAdcBits(getSampleRate()) << " bits, API signed-16, IF GR "
        << config_.sdrplay.gainReduction << " dB, LNA state " << config_.sdrplay.lnaState
        << ", USB " << (config_.sdrplay.usbBulk ? "bulk" : "isoch");
    return true;
}

bool SdrplayDevice::start() {
    if (!selected_ || initialized_) return false;
    haveSequence_ = false;
    sdrplay_api_CallbackFnsT callbacks{};
    callbacks.StreamACbFn = streamCallback;
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
            self.gaps_.fetch_add(1);
            if constexpr (Metrics::Enabled) Metrics::registry().sdrplayGaps.inc();
            const uint32_t missing = p->firstSampleNum - self.nextSample_;
            self.missing_.fetch_add(missing);
            // A forward modular delta below half the counter range retains
            // the sample clock. Backward/ambiguous jumps require recovery.
            // Raw capture writers deliberately decline segmentation.
            if (missing >= 0x80000000u || !self.m_bufferWriter.discontinuity(uint64_t(missing) * 2)) {
                self.fail("sample_gap"); return;
            }
        }
        self.haveSequence_ = true;
        self.nextSample_ = uint32_t(p->firstSampleNum + count);
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
    return {{"device", "RSP1B"}, {"serial", device_.SerNo}, {"api_version", std::to_string(apiVersion_)},
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
