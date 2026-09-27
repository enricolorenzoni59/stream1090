/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "devices/DeviceSession.hpp"
#include "RingBuffer.hpp"
#include <atomic>
#include <future>
#include <stdexcept>
struct Device {
    IAsyncWriter<int>& writer;
    std::future<size_t> producer;
    std::atomic<bool>& watcherDone;
    bool& destroyed;
    bool closed=false;
    Device(IAsyncWriter<int>& w, std::atomic<bool>& done, bool& d) : writer(w), watcherDone(done), destroyed(d) {}
    void close() {
        if (closed) return;
        if (!watcherDone.load()) throw std::runtime_error("watchdog still accesses device");
        if (producer.valid() && producer.get() >= 64) throw std::runtime_error("cancelled write reported success");
        closed=true;
    }
    ~Device() { writer.shutdown(); destroyed=true; }
};
int main() {
    std::unique_ptr<Device> device;
    bool destroyed=false;
    {
        RingBufferAsync<int,4,2> ring;
        RingBufferAsync<int,4,2>::Writer writer(ring);
        std::atomic<bool> done{false};
        DeviceSession session(device,writer);
        device=std::make_unique<Device>(writer,done,destroyed);
        destroyed=false;
        session.watchdog=std::jthread([&](std::stop_token stop) {
            while(!stop.stop_requested()) std::this_thread::yield();
            done=true;
        });
        device->producer=std::async(std::launch::async,[&] { int data[64]{}; return writer.write(data,64); });
        if(device->producer.wait_for(std::chrono::milliseconds(20)) != std::future_status::timeout)
            throw std::runtime_error("producer should block on full ring");
        session.stop(); session.stop();
    }
    if(device || !destroyed) throw std::runtime_error("device outlived writer");
    // Early-return/exception before starting a watchdog is safe as well.
    {
        RingBufferAsync<int,4,2> ring; RingBufferAsync<int,4,2>::Writer writer(ring);
        std::atomic<bool> done{true};
        DeviceSession session(device,writer);
        device=std::make_unique<Device>(writer,done,destroyed);
    }
}
