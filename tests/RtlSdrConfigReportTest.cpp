// Fake driver: exercise the real config hooks without USB hardware or RF writes.
#include <cstring>
#include <vector>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include "devices/RtlSdrDevice.hpp"
#include "Logger.hpp"
struct rtlsdr_dev {
    int unused;
} device;
int modeRC = 0, bwRC = 0, modeCalls = 0, gainCalls = 0, lnaCalls = 0, lna = 0;
uint32_t bandwidth = 0;
std::string usbSerial = "00000008";
std::vector<std::string> calls;
extern "C" {
void rtlsdr_set_log_callback(rtlsdr_log_callback_t) {}
void rtlsdr_mark_dev_lost(rtlsdr_dev_t*) {}
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t*) {
    return 1090000000;
}
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t*) {
    return 2400000;
}
uint32_t rtlsdr_get_device_count() {
    return 1;
}
int rtlsdr_get_device_usb_strings(uint32_t, char* m, char* p, char* s) {
    if (m)
        std::strcpy(m, "Fake");
    if (p)
        std::strcpy(p, "R820T");
    if (s)
        std::strcpy(s, usbSerial.c_str());
    return 0;
}
int rtlsdr_open(rtlsdr_dev_t** d, uint32_t) {
    *d = &device;
    return 0;
}
int rtlsdr_close(rtlsdr_dev_t*) {
    return 0;
}
int rtlsdr_set_center_freq(rtlsdr_dev_t*, uint32_t) {
    calls.push_back("frequency");
    return 0;
}
int rtlsdr_set_sample_rate(rtlsdr_dev_t*, uint32_t) {
    return 0;
}
int rtlsdr_reset_buffer(rtlsdr_dev_t*) {
    return 0;
}
rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t*) {
    return RTLSDR_TUNER_R820T;
}
int rtlsdr_read_async(rtlsdr_dev_t*, rtlsdr_read_async_cb_t, void*, uint32_t, uint32_t) {
    return 0;
}
int rtlsdr_cancel_async(rtlsdr_dev_t*) {
    return 0;
}
int rtlsdr_get_tuner_gains(rtlsdr_dev_t*, int* g) {
    int a[] = {0, 364, 496};
    if (g)
        std::memcpy(g, a, sizeof(a));
    return 3;
}
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t*, int) {
    calls.push_back("mode");
    ++modeCalls;
    return modeRC;
}
int rtlsdr_set_tuner_gain(rtlsdr_dev_t*, int) {
    calls.push_back("gain");
    ++gainCalls;
    lna = 10;
    return 0;
}
int rtlsdr_set_agc_mode(rtlsdr_dev_t*, int) {
    calls.push_back("agc");
    return 0;
}
int rtlsdr_set_bias_tee(rtlsdr_dev_t*, int) {
    calls.push_back("bias_tee");
    return 0;
}
int rtlsdr_set_freq_correction(rtlsdr_dev_t*, int) {
    calls.push_back("ppm");
    return 0;
}
int rtlsdr_set_offset_tuning(rtlsdr_dev_t*, int) {
    calls.push_back("offset_tuning");
    return 0;
}
int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t*, uint32_t b) {
    calls.push_back("bandwidth");
    if (!bwRC)
        bandwidth = b;
    return bwRC;
}
int rtlsdr_r82xx_set_lna_gain(rtlsdr_dev_t*, int g) {
    ++lnaCalls;
    lna = g;
    return 0;
}
int rtlsdr_r82xx_set_mixer_gain(rtlsdr_dev_t*, int) {
    return 0;
}
int rtlsdr_r82xx_set_vga_gain(rtlsdr_dev_t*, int) {
    return 0;
}
}
struct Writer : IAsyncWriter<uint8_t> {
    size_t write(const uint8_t*, size_t n) override {
        return n;
    }
    void shutdown() override {}
};

void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

std::string configure(RtlSdrDevice& d, const DeviceConfig& cfg) {
    std::ostringstream captured;
    auto* old = std::cerr.rdbuf(captured.rdbuf());
    try {
        d.applyConfigPostOpen(cfg);
    } catch (...) {
        std::cerr.rdbuf(old);
        throw;
    }
    std::cerr.rdbuf(old);
    return captured.str();
}
void contains(const std::string& text, const std::string& part) {
    if (text.find(part) == std::string::npos)
        throw std::runtime_error("missing: " + part + " in:\n" + text);
}
int main() {
    try {
        Writer w;
        Log::setLevel(Log::Level::MSG); // normal non-verbose logging on this base
        {
            RtlSdrDevice d(Rate_2_4_Mhz, w);
            require(d.open(), "open");
            calls.clear();
            DeviceConfig cfg;
            cfg.gainDb = 36;
            cfg.tunerBandwidth = 3000000;
            auto log = configure(d, cfg);
            contains(log, "configuration startup: report");
            contains(log, "serial=\"00000008\"");
            contains(log, "gain requested=\"36\"; call: argument=36.4 dB nominal step; API accepted");
            contains(log, "agc requested=\"off\"; no recorded API call; current=unverified");
            contains(log, "effective analog bandwidth unavailable");
            require(calls == std::vector<std::string>({"mode", "gain", "bandwidth"}), "startup call order");
            calls.clear();
            log = configure(d, cfg);
            contains(log, "configuration reload: report");
            require(calls.empty(), "identical config must not issue writes");
            bwRC = -7;
            cfg.tunerBandwidth = 1000000;
            log = configure(d, cfg);
            contains(log, "incomplete");
            contains(log, "API failed rc=-7; current=unconfirmed");
            cfg.tunerBandwidth.reset();
            calls.clear();
            log = configure(d, cfg);
            contains(log, "tuner_bandwidth omitted (retained; no reset)");
            contains(log, "incomplete");
            require(calls.empty(), "omitted bandwidth retried");
            bwRC = 0;
            cfg.tunerBandwidth = 0;
            log = configure(d, cfg);
            contains(log, "configuration reload: report");
            contains(log, "argument=0 Hz (0=auto); API accepted");
            d.close();
        }
        {
            RtlSdrDevice d(Rate_2_4_Mhz, w);
            require(d.open(), "open");
            calls.clear();
            DeviceConfig cfg;
            cfg.gainDb = 0;
            cfg.tunerBandwidth = 0;
            auto log = configure(d, cfg);
            contains(log, "argument=0.0 dB nominal step; API accepted");
            require(calls == std::vector<std::string>({"mode", "gain", "bandwidth"}),
                    "new-base zero settings must apply");
            modeRC = -9;
            cfg.gainDb = 36;
            calls.clear();
            log = configure(d, cfg);
            contains(log, "incomplete");
            contains(log, "argument=manual; API failed rc=-9");
            require(calls == std::vector<std::string>({"mode"}), "new-base mode failure must short circuit");
            modeRC = 0;
            d.close();
        }
        {
            usbSerial = "test\nserial";
            RtlSdrDevice d(Rate_2_4_Mhz, w);
            require(d.open(), "open");
            calls.clear();
            DeviceConfig cfg;
            cfg.frequencyHz = 1089000000;
            cfg.agc = true;
            cfg.gainDb = 36;
            cfg.tunerBandwidth = 3000000;
            cfg.biasTee = true;
            cfg.offsetTuning = true;
            cfg.ppm = 2;
            auto log = configure(d, cfg);
            require(calls == std::vector<std::string>(
                                 {"frequency", "agc", "mode", "gain", "bandwidth", "bias_tee", "offset_tuning", "ppm"}),
                    "typed config setter order changed");
            contains(log, "test\\x0aserial");
            contains(log, "on (digital AGC); API accepted");
            cfg.lnaGain = 7;
            log = configure(d, cfg);
#ifdef STREAM1090_HAVE_RTLSDR_BLOG
            contains(log, "total gain unavailable after stage call");
            contains(log, "lna_gain requested=\"7\"; call: argument=7 (stage index); API accepted");
#else
            contains(log, "setting rejected: lna_gain");
            contains(log, "incomplete");
#endif
            d.close();
        }
        {
            RtlSdrDevice d(Rate_2_4_Mhz, w);
            require(d.open(), "open");
            auto cfg = applyBackendDefaults(DeviceConfig{}, InputDeviceType::RTLSDR, Rate_2_4_Mhz);
            auto log = configure(d, cfg);
            contains(log, "adaptive_gain=enabled; auto_ppm=enabled");
            contains(log, "gain requested=\"49.6\"");
            // Runtime calls (same setters used by maintenance) must enter the history.
            require(d.setPpm(3), "runtime ppm");
            require(d.setGain(36), "runtime gain");
            cfg.gainDb.reset();
            cfg.ppm.reset();
            log = configure(d, cfg);
            contains(log, "history: argument=36.4 dB nominal step; API accepted");
            contains(log, "history: argument=3; API accepted");
            d.close();
            require(d.open(), "reopen");
            DeviceConfig fresh;
            log = configure(d, fresh);
            contains(log, "configuration startup: report");
            contains(log, "gain omitted; no recorded API call; current=unverified");
            d.close();
        }
        std::cout << "RTL configuration diagnostics passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
