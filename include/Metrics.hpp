/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#pragma once

#include "Stats.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#endif

/*
 * The scrapeable view of the process.
 *
 * Two kinds of counter live here. Everything the demodulator records sits in
 * Stats::Counters, which the demodulation thread owns exclusively and copies
 * into this registry about once a second: the sample loop must not pay for an
 * atomic per event. Everything else, device, output, network and logging,
 * happens a few thousand times a second at most and writes here directly
 * through relaxed atomics.
 *
 * Readers may therefore observe two counters from either side of a publish.
 * That is deliberate: Prometheus reads every series independently anyway, and
 * a torn pair costs one scrape of skew, whereas a lock would put the HTTP
 * thread in the way of the demodulator.
 */
namespace Metrics {

#if defined(STREAM1090_METRICS) && STREAM1090_METRICS
inline constexpr bool Enabled = true;
#else
inline constexpr bool Enabled = false;
#endif

struct Counter {
    std::atomic<uint64_t> value { 0 };

    void inc(uint64_t n = 1) noexcept {
        if constexpr (Enabled)
            value.fetch_add(n, std::memory_order_relaxed);
    }

    uint64_t get() const noexcept { return value.load(std::memory_order_relaxed); }
};

struct Gauge {
    std::atomic<double> value { 0.0 };

    void set(double v) noexcept {
        if constexpr (Enabled)
            value.store(v, std::memory_order_relaxed);
    }

    double get() const noexcept { return value.load(std::memory_order_relaxed); }
};

class Registry {
  public:
    // ---- identity ---------------------------------------------------------
    void setBuildInfo(const std::string& version, const std::string& pipeline, uint32_t inputRate,
                      uint32_t outputRate, uint32_t streams) {
        m_version = version;
        m_pipeline = pipeline;
        m_inputRate = inputRate;
        m_outputRate = outputRate;
        m_streams = streams;
    }

    void setDeviceName(const std::string& name) { m_deviceName = name; }

    // ---- device -----------------------------------------------------------
    Gauge deviceUp;
    Gauge deviceLastSampleAge;
    Gauge settingGainDb;
    Gauge settingPpm;
    Gauge settingFrequencyHz;
    Gauge settingAgc;
    Gauge configGeneration;
    Counter configReloadsOk;
    Counter configReloadsFailed;

    // ---- watchdog and sample drops ---------------------------------------
    Counter watchdogLate;
    Counter watchdogLost;
    Counter sampleDropEvents;
    Gauge sampleDropDeficit;
    Gauge sampleDropWorst;

    // ---- demodulator (published from the demodulation thread) -------------
    std::array<std::atomic<uint64_t>, Stats::NUM_EVENTS> demodEvents {};
    std::array<std::atomic<uint64_t>, Stats::NumDF> demodSent {};
    std::array<std::atomic<uint64_t>, Stats::NumDF> demodDups {};

    /// Copies one cumulative counter snapshot in. Called by the demodulation
    /// thread on its own tick, roughly once a second.
    void publishDemod(const Stats::Counters& counters) {
        if constexpr (!Enabled)
            return;

        for (size_t i = 0; i < counters.events.size(); i++)
            demodEvents[i].store(counters.events[i], std::memory_order_relaxed);
        for (size_t i = 0; i < Stats::NumDF; i++) {
            demodSent[i].store(counters.sent[i], std::memory_order_relaxed);
            demodDups[i].store(counters.dups[i], std::memory_order_relaxed);
        }
        m_publishSteady.store(steadySeconds(), std::memory_order_relaxed);
    }

    // ---- AVR output -------------------------------------------------------
    Counter outputShortFrames;
    Counter outputLongFrames;
    Counter outputBytes;
    Counter outputErrors;

    // ---- TCP output -------------------------------------------------------
    Gauge tcpClients;
    Counter tcpFramesDropped;
    Counter tcpSlowDisconnects;

    // ---- logging ----------------------------------------------------------
    std::array<Counter, 5> logMessages;

    // ---- endpoint self observability -------------------------------------
    Counter scrapes;
    Gauge scrapeDurationSeconds;

    Registry() {
        m_processStart = std::chrono::system_clock::now();
        m_publishSteady.store(0.0, std::memory_order_relaxed);
    }

    /// Seconds since the last demodulator publish. Reads zero before the first
    /// snapshot, then grows without bound the moment the demodulation thread
    /// stops, which is the only way to tell a wedged pipeline from a quiet sky.
    double snapshotAge() const {
        const double last = m_publishSteady.load(std::memory_order_relaxed);
        if (last == 0.0)
            return 0.0;

        return steadySeconds() - last;
    }

    double uptimeSeconds() const {
        return std::chrono::duration<double>(std::chrono::system_clock::now() - m_processStart).count();
    }

    double startTimeUnix() const {
        return std::chrono::duration<double>(m_processStart.time_since_epoch()).count();
    }

    bool ready() const {
        return deviceUp.get() > 0.5 && snapshotAge() < 5.0;
    }

    const std::string& version() const { return m_version; }
    const std::string& pipeline() const { return m_pipeline; }
    const std::string& deviceName() const { return m_deviceName; }
    uint32_t inputRate() const { return m_inputRate; }
    uint32_t outputRate() const { return m_outputRate; }
    uint32_t streams() const { return m_streams; }

    static double steadySeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

  private:
    std::chrono::system_clock::time_point m_processStart;
    std::atomic<double> m_publishSteady { 0.0 };
    std::string m_version { "unknown" };
    std::string m_pipeline { "unknown" };
    std::string m_deviceName { "none" };
    uint32_t m_inputRate { 0 };
    uint32_t m_outputRate { 0 };
    uint32_t m_streams { 0 };
};

inline Registry& registry() {
    static Registry instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Rendering, Prometheus text exposition format 0.0.4
// ---------------------------------------------------------------------------

namespace render_detail {

inline void number(std::string& out, double v) {
    if (v == double(int64_t(v)) && v < 9.2e18 && v > -9.2e18) {
        out += std::to_string(int64_t(v));
        return;
    }
    char buf[40];
    // Enough significant digits that a unix timestamp keeps its sub-second part
    // instead of collapsing to 1.78681e+09.
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    out += buf;
}

inline void number(std::string& out, uint64_t v) {
    out += std::to_string(v);
}

inline void head(std::string& out, const char* name, const char* help, const char* type) {
    out += "# HELP stream1090_";
    out += name;
    out += ' ';
    out += help;
    out += "\n# TYPE stream1090_";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

/// Label values come from our own tables, never from the air, so there is
/// nothing to escape beyond what we write ourselves.
inline void sample(std::string& out, const char* name, const std::string& labels, double value) {
    out += "stream1090_";
    out += name;
    out += labels;
    out += ' ';
    number(out, value);
    out += '\n';
}

inline std::string labels(std::initializer_list<std::pair<const char*, std::string>> kv) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : kv) {
        if (!first)
            out += ',';
        first = false;
        out += k;
        out += "=\"";
        out += v;
        out += '"';
    }
    out += '}';
    return out;
}

inline uint64_t load(const std::atomic<uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

/// Names for every Stats event, in enum order. Used for the generic
/// demod_events_total family so a new counter shows up without a render edit.
inline constexpr std::array<const char*, Stats::NUM_EVENTS> EventNames { {
    "num_streams",
    "num_iterations",
    "df17_header",
    "df17_good_message",
    "df17_bad_message",
    "df17_repair_success",
    "df17_repair_failed",
    "df17_first_sighting",
    "df17_first_confirmed",
    "comm_b_good_message",
    "acas_surv_good_message",
    "df11_header",
    "df11_icao_ca_found",
    "df11_icao_ca_found_good_crc",
    "df11_icao_ca_found_1_bit_fix",
    "df11_icao_ca_found_bad_crc",
    "df11_new_good_crc",
    "short_header",
    "short_addr_unknown",
    "short_addr_not_alive",
    "long_cb_header",
    "long_cb_addr_unknown",
    "long_cb_addr_not_alive",
    "reject_altitude",
    "reject_squawk",
    "altitude_rescued",
    "squawk_rescued",
    "noise_floor_rejected",
} };

struct EventMetric {
    const char* metric;
    const char* label;
    const char* value;
    Stats::EventType event;
};

/// A small set of friendly aliases on top of the generic family, so the rules
/// file and dashboards do not have to spell the enum names.
inline constexpr std::array<EventMetric, 12> EventMetrics { {
    { "es_frames_total", "result", "crc_ok", Stats::DF17_GOOD_MESSAGE },
    { "es_frames_total", "result", "crc_bad", Stats::DF17_BAD_MESSAGE },
    { "es_frames_total", "result", "repair_ok", Stats::DF17_REPAIR_SUCCESS },
    { "es_frames_total", "result", "repair_failed", Stats::DF17_REPAIR_FAILED },
    { "df11_total", "result", "good_crc", Stats::DF11_ICAO_CA_FOUND_GOOD_CRC },
    { "df11_total", "result", "one_bit_fix", Stats::DF11_ICAO_CA_FOUND_1_BIT_FIX },
    { "df11_total", "result", "bad_crc", Stats::DF11_ICAO_CA_FOUND_BAD_CRC },
    { "df11_total", "result", "new", Stats::DF11_NEW_GOOD_CRC },
    { "frames_rejected_total", "reason", "altitude", Stats::REJECT_ALTITUDE },
    { "frames_rejected_total", "reason", "squawk", Stats::REJECT_SQUAWK },
    { "frames_rejected_total", "reason", "noise_floor", Stats::NOISE_FLOOR_REJECTED },
    { "frames_rescued_total", "reason", "altitude", Stats::ALTITUDE_RESCUED },
} };

struct MetricHelp {
    const char* metric;
    const char* help;
};

inline constexpr std::array<MetricHelp, 5> EventHelp { {
    { "es_frames_total", "Extended squitter frame positions by checksum outcome. Their sum is the trigger count." },
    { "df11_total", "All call replies by outcome." },
    { "frames_rejected_total", "Frames dropped by a field plausibility or signal gate." },
    { "frames_rescued_total", "Frames a held first sighting recovered after a plausibility gate rejected them." },
    { "demod_events_total", "Raw demodulator counters, one series per Stats event." },
} };

inline void headRaw(std::string& out, const char* name, const char* help, const char* type) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += "\n# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

inline void sampleRaw(std::string& out, const char* name, double value) {
    out += name;
    out += ' ';
    number(out, value);
    out += '\n';
}

/// The conventional process_* collector every Prometheus dashboard expects.
/// Linux only: everything here comes out of /proc, and there is no point
/// inventing a portable shim for numbers nobody scrapes on macOS.
inline void processMetrics(std::string& out, const Registry& reg) {
    headRaw(out, "process_start_time_seconds", "Unix time the process started.", "gauge");
    sampleRaw(out, "process_start_time_seconds", reg.startTimeUnix());

#ifdef __linux__
    if (FILE* f = std::fopen("/proc/self/stat", "r")) {
        char buf[2048];
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';

        // The comm field may contain spaces and parentheses, so parsing starts
        // after the last closing one. Field 3 is a letter, so the tokens are
        // walked by hand and only the wanted ones converted.
        const char* p = std::strrchr(buf, ')');
        if (p != nullptr) {
            double fields[24] = {};
            size_t count = 0;
            p++;
            while (count < 24 && *p != '\0') {
                while (*p == ' ')
                    p++;
                if (*p == '\0')
                    break;

                const char* token = p;
                while (*p != '\0' && *p != ' ')
                    p++;

                fields[count++] = std::strtod(token, nullptr);
            }

            if (count > 21) {
                const double ticks = double(sysconf(_SC_CLK_TCK));
                const double page = double(sysconf(_SC_PAGESIZE));
                headRaw(out, "process_cpu_seconds_total", "Total user and system CPU time.", "counter");
                sampleRaw(out, "process_cpu_seconds_total",
                          (fields[11] + fields[12]) / (ticks > 0 ? ticks : 100.0));
                headRaw(out, "process_virtual_memory_bytes", "Virtual memory size.", "gauge");
                sampleRaw(out, "process_virtual_memory_bytes", fields[20]);
                headRaw(out, "process_resident_memory_bytes", "Resident memory size.", "gauge");
                sampleRaw(out, "process_resident_memory_bytes", fields[21] * page);
            }
        }
    }

    if (DIR* dir = ::opendir("/proc/self/fd")) {
        double open = 0;
        while (::readdir(dir) != nullptr)
            open++;
        ::closedir(dir);
        headRaw(out, "process_open_fds", "Open file descriptors.", "gauge");
        sampleRaw(out, "process_open_fds", open > 2 ? open - 2 : 0);
    }
#else
    (void)out;
#endif
}

} // namespace render_detail

/// Renders the whole registry. Called on the metrics thread only.
inline std::string render(Registry& reg) {
    using namespace render_detail;
    const double renderStart = Registry::steadySeconds();

    std::string out;
    out.reserve(16384);

    // ---- identity ---------------------------------------------------------
    head(out, "build_info", "Build and pipeline identity, always 1.", "gauge");
    sample(out, "build_info",
           labels({
               { "version", reg.version() },
               { "pipeline", reg.pipeline() },
               { "device", reg.deviceName() },
               { "input_rate_hz", std::to_string(reg.inputRate()) },
               { "output_rate_hz", std::to_string(reg.outputRate()) },
               { "streams", std::to_string(reg.streams()) },
           }),
           1.0);

    head(out, "start_time_seconds", "Unix time the process started.", "gauge");
    sample(out, "start_time_seconds", "", reg.startTimeUnix());
    head(out, "uptime_seconds", "Seconds since the process started.", "gauge");
    sample(out, "uptime_seconds", "", reg.uptimeSeconds());

    // ---- device and ingest ------------------------------------------------
    head(out, "device_up", "1 when the input device is running.", "gauge");
    sample(out, "device_up", "", reg.deviceUp.get());
    head(out, "device_last_sample_age_seconds", "Age of the last batch of samples the device delivered.", "gauge");
    sample(out, "device_last_sample_age_seconds", "", reg.deviceLastSampleAge.get());
    head(out, "device_setting", "Device configuration currently applied.", "gauge");
    sample(out, "device_setting", labels({ { "setting", "gain_db" } }), reg.settingGainDb.get());
    sample(out, "device_setting", labels({ { "setting", "ppm" } }), reg.settingPpm.get());
    sample(out, "device_setting", labels({ { "setting", "frequency_hz" } }), reg.settingFrequencyHz.get());
    sample(out, "device_setting", labels({ { "setting", "agc" } }), reg.settingAgc.get());
    head(out, "config_generation", "Increments on every accepted configuration reload.", "gauge");
    sample(out, "config_generation", "", reg.configGeneration.get());
    head(out, "config_reloads_total", "SIGHUP configuration reloads by outcome.", "counter");
    sample(out, "config_reloads_total", labels({ { "result", "ok" } }), double(reg.configReloadsOk.get()));
    sample(out, "config_reloads_total", labels({ { "result", "failed" } }), double(reg.configReloadsFailed.get()));

    head(out, "watchdog_events_total", "Watchdog observations about device liveness.", "counter");
    sample(out, "watchdog_events_total", labels({ { "event", "late" } }), double(reg.watchdogLate.get()));
    sample(out, "watchdog_events_total", labels({ { "event", "lost" } }), double(reg.watchdogLost.get()));

    head(out, "sample_drop_events_total", "Sample drop events reported by the device layer.", "counter");
    sample(out, "sample_drop_events_total", "", double(reg.sampleDropEvents.get()));
    head(out, "sample_drop_deficit_pairs", "Cumulative IQ pairs the device layer knows it lost.", "gauge");
    sample(out, "sample_drop_deficit_pairs", "", reg.sampleDropDeficit.get());
    head(out, "sample_drop_worst_deficit_pairs", "Worst single drop gap observed this run.", "gauge");
    sample(out, "sample_drop_worst_deficit_pairs", "", reg.sampleDropWorst.get());

    // ---- demodulator counters --------------------------------------------
    head(out, "demod_events_total", "Raw demodulator counters, one series per Stats event.", "counter");
    for (size_t i = 0; i < Stats::NUM_EVENTS; i++) {
        const auto v = load(reg.demodEvents[i]);
        if (v == 0)
            continue;
        sample(out, "demod_events_total", labels({ { "event", EventNames[i] } }), double(v));
    }

    for (const auto& help : EventHelp) {
        if (std::string_view(help.metric) == "demod_events_total")
            continue;
        head(out, help.metric, help.help, "counter");
        for (const auto& metric : EventMetrics) {
            if (std::string_view(metric.metric) != help.metric)
                continue;
            sample(out, metric.metric, labels({ { metric.label, metric.value } }),
                   double(load(reg.demodEvents[metric.event])));
        }
    }

    head(out, "messages_total", "Frames accepted and handed to the output, by downlink format.", "counter");
    for (size_t df = 0; df < Stats::NumDF; df++) {
        const auto v = load(reg.demodSent[df]);
        if (v == 0)
            continue;
        sample(out, "messages_total", labels({ { "df", std::to_string(df) } }), double(v));
    }

    head(out, "messages_duplicate_total",
         "Frames dropped because the same address was heard within the duplicate window.", "counter");
    for (size_t df = 0; df < Stats::NumDF; df++) {
        const auto v = load(reg.demodDups[df]);
        if (v == 0)
            continue;
        sample(out, "messages_duplicate_total", labels({ { "df", std::to_string(df) } }), double(v));
    }

    // ---- AVR output -------------------------------------------------------
    head(out, "output_frames_total", "Frames written to the AVR output.", "counter");
    sample(out, "output_frames_total", labels({ { "kind", "short" } }), double(reg.outputShortFrames.get()));
    sample(out, "output_frames_total", labels({ { "kind", "long" } }), double(reg.outputLongFrames.get()));
    head(out, "output_bytes_total", "Bytes written to the AVR output.", "counter");
    sample(out, "output_bytes_total", "", double(reg.outputBytes.get()));
    head(out, "output_errors_total", "Failed writes to the AVR output.", "counter");
    sample(out, "output_errors_total", "", double(reg.outputErrors.get()));

    // ---- TCP output -------------------------------------------------------
    head(out, "tcp_clients", "Clients currently connected to an AVR or Beast TCP listener.", "gauge");
    sample(out, "tcp_clients", "", reg.tcpClients.get());
    head(out, "tcp_frames_dropped_total",
         "Frames dropped because the network ring or a client buffer was full.", "counter");
    sample(out, "tcp_frames_dropped_total", "", double(reg.tcpFramesDropped.get()));
    head(out, "tcp_slow_client_disconnects_total",
         "Clients disconnected because their pending output exceeded the buffer bound.", "counter");
    sample(out, "tcp_slow_client_disconnects_total", "", double(reg.tcpSlowDisconnects.get()));

    // ---- logging ----------------------------------------------------------
    static constexpr std::array<const char*, 5> LevelNames { "error", "warn", "msg", "info", "debug" };
    head(out, "log_messages_total", "Log lines emitted by level.", "counter");
    for (size_t i = 0; i < LevelNames.size(); i++)
        sample(out, "log_messages_total", labels({ { "level", LevelNames[i] } }),
               double(reg.logMessages[i].get()));

    // ---- endpoint ---------------------------------------------------------
    head(out, "metrics_snapshot_age_seconds",
         "Age of the demodulator counter snapshot. Stays near zero while the pipeline runs; a growing "
         "value with a live device means the demodulation thread is wedged.",
         "gauge");
    sample(out, "metrics_snapshot_age_seconds", "", reg.snapshotAge());
    head(out, "metrics_scrapes_total", "Scrapes served.", "counter");
    sample(out, "metrics_scrapes_total", "", double(reg.scrapes.get()));
    head(out, "metrics_scrape_duration_seconds", "Time to render the previous scrape.", "gauge");
    sample(out, "metrics_scrape_duration_seconds", "", reg.scrapeDurationSeconds.get());

    processMetrics(out, reg);

    reg.scrapes.inc();
    reg.scrapeDurationSeconds.set(Registry::steadySeconds() - renderStart);
    return out;
}

} // namespace Metrics
