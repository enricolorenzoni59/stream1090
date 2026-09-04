#include "MainInstance.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

struct FakeDevice {
    std::mutex mutex;
    std::condition_variable cv;
    bool applying = false;
    bool allowApplyToFinish = false;
    bool closed = false;
    bool overlap = false;

    void reload() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            applying = true;
            cv.notify_all();
        }
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return allowApplyToFinish; });
        applying = false;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        if (applying)
            overlap = true;
        closed = true;
    }
};

} // namespace

int main() {
    FakeDevice device;
    std::thread watchdog([&] { device.reload(); });
    {
        std::unique_lock<std::mutex> lock(device.mutex);
        device.cv.wait(lock, [&] { return device.applying; });
    }

    // This is the SIGHUP/SIGINT interleaving: joining first must wait for the
    // in-flight reload before the device handle is closed.
    std::thread release([&] {
        std::lock_guard<std::mutex> lock(device.mutex);
        device.allowApplyToFinish = true;
        device.cv.notify_all();
    });
    closeAfterWatchdog(device, watchdog);
    release.join();

    if (!device.closed || device.overlap)
        return 1;
    return 0;
}
