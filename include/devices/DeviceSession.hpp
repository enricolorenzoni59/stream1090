/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <memory>
#include <thread>

// Declare after the writer. All exits (including exceptions and failed start)
// quiesce the producer and destroy the device while its writer is still alive.
template<class Device, class Writer> class DeviceSession {
  public:
    DeviceSession(std::unique_ptr<Device>& device, Writer& writer) : device_(device), writer_(writer) {}
    ~DeviceSession() { stop(); device_.reset(); }
    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;
    std::jthread watchdog;
    void stop() {
        if (stopped_) return;
        writer_.shutdown();
        watchdog.request_stop();
        if (watchdog.joinable()) watchdog.join();
        if (device_) device_->close();
        stopped_ = true;
    }
  private:
    std::unique_ptr<Device>& device_;
    Writer& writer_;
    bool stopped_ = false;
};
