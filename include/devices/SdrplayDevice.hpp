/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <sdrplay_api.h>
#include "devices/InputDeviceBase.hpp"
#include <array>
#include "SdrplayIqStats.hpp"

class SdrplayDevice final : public InputDeviceBase<int16_t> {
  public:
    SdrplayDevice(SampleRate rate, IAsyncWriter<int16_t>& writer) : InputDeviceBase(rate, writer) {}
    ~SdrplayDevice() override { close(); }
    bool open() override;
    bool start() override;
    void stop() override;
    void close() override;
    void applyConfigPreOpen(const DeviceConfig& config) override { config_ = config; }
    bool streamFailed() const override { return failed_.load(); }
    bool usesExactSampleCounter() const override { return true; }
    std::map<std::string, std::string> captureMetadata() const override;
  private:
    static void streamCallback(short*, short*, sdrplay_api_StreamCbParamsT*, unsigned, unsigned, void*) noexcept;
    static void eventCallback(sdrplay_api_EventT, sdrplay_api_TunerSelectT, sdrplay_api_EventParamsT*, void*) noexcept;
    bool check(sdrplay_api_ErrT, const char*) const;
    void release();
    void invalidateTelemetry() noexcept;
    void publishTelemetry(const SdrplayIqStats::Window&) noexcept;
    void fail(const char* reason) noexcept {
        failureReason_.store(reason);
        failed_.store(true); m_running.store(false); shutdownWriter();
        invalidateTelemetry();
    }
    DeviceConfig config_;
    sdrplay_api_DeviceT device_{};
    sdrplay_api_DeviceParamsT* params_ = nullptr;
    bool apiOpen_ = false, selected_ = false, initialized_ = false;
    float apiVersion_ = 0;
    std::atomic<bool> failed_{false};
    std::atomic<const char*> failureReason_{"none"};
    std::atomic<uint64_t> callbacks_{0}, resets_{0}, gaps_{0}, missing_{0}, overloads_{0};
    bool haveSequence_ = false; // Stream A callback thread only
    uint32_t nextSample_ = 0;
    SdrplayIqStats iqStats_; // Stream A callback ownership, reset before Init
    std::array<int16_t, 8192> interleaved_{};
};
