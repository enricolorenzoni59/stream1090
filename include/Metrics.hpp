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

    /// For counters whose source already keeps the total (the TCP server's
    /// own atomics). Keeps the exported series monotonic without adding up
    /// deltas in the caller.
    void set(uint64_t v) noexcept {
        if constexpr (Enabled)
            value.store(v, std::memory_order_relaxed);
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

struct SignalQuality {
    double signalDbfs { 0.0 };
    double noiseDbfs { 0.0 };
    double snrDb { 0.0 };
};

/// A fixed-bucket histogram. There is one more slot than there are bounds: the
/// last one is the +Inf overflow. Updates come from the DSP thread, reads from
/// the metrics thread, so every slot is atomic.
template <size_t NumBuckets> struct Histogram {
    std::array<std::atomic<uint64_t>, NumBuckets + 1> buckets {};
    std::atomic<uint64_t> count { 0 };
    std::atomic<double> sum { 0.0 };

    template <size_t N> void observe(double value, const std::array<double, N>& bounds) noexcept {
        static_assert(N == NumBuckets, "one bucket per bound");
        if constexpr (!Enabled) {
            (void)value;
            (void)bounds;
            return;
        }
        size_t bucket = 0;
        while (bucket < N && value > bounds[bucket])
            ++bucket;
        buckets[bucket].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(value, std::memory_order_relaxed);
    }
};

inline constexpr std::array<double, 8> RssiRatioBounds { 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0 };
inline constexpr std::array<double, 25> DbfsBounds {
    -72.0, -69.0, -66.0, -63.0, -60.0, -57.0, -54.0, -51.0, -48.0, -45.0, -42.0, -39.0, -36.0,
    -33.0, -30.0, -27.0, -24.0, -21.0, -18.0, -15.0, -12.0, -9.0, -6.0, -3.0, 0.0
};
inline constexpr std::array<double, 16> SnrBounds {
    0.0, 3.0, 6.0, 9.0, 12.0, 15.0, 18.0, 21.0, 24.0, 27.0, 30.0, 33.0, 36.0, 39.0, 42.0, 45.0
};
inline constexpr std::array<double, 8> PreambleScoreBounds { 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0 };

class Registry {
  public:
    // ---- identity ---------------------------------------------------------
    void setBuildInfo(const std::string& version, const std::string& pipeline, uint32_t inputRate,
                      uint32_t outputRate, uint32_t streams, const std::string& commit = std::string()) {
        m_version = version;
        m_pipeline = pipeline;
        m_inputRate = inputRate;
        m_outputRate = outputRate;
        m_streams = streams;
        m_commit = commit.empty() ? std::string("unknown") : commit;
    }

    void setDeviceName(const std::string& name) { m_deviceName = name; }

    // ---- device -----------------------------------------------------------
    Gauge deviceUp;
    Gauge deviceLastSampleAge;
    Gauge settingPpm;
    Gauge settingFrequencyHz;
    Gauge settingAgc;
    Gauge configGeneration;
    Counter configReloadsOk;
    Counter configReloadsFailed;

    // ---- RTL-SDR automatic PPM calibration -------------------------------
    Gauge rtlAutoPpmEnabled;
    Gauge rtlAutoPpmCorrection;
    Gauge rtlAutoPpmLastObservation;
    Gauge rtlAutoPpmMedianResidual;
    Gauge rtlAutoPpmEstimatedError;
    Gauge rtlAutoPpmSampleRateHz;
    Gauge rtlAutoPpmWindowsCollected;
    Gauge rtlAutoPpmWindowSeconds;
    Gauge rtlAutoPpmTargetWindows;
    Gauge rtlAutoPpmWarmupSeconds;
    Gauge rtlAutoPpmDeadband;
    Gauge rtlAutoPpmMaxStep;
    Gauge rtlAutoPpmLimit;
    Gauge rtlAutoPpmLastEstimateSteady;
    Counter rtlAutoPpmWindowsClean;
    Counter rtlAutoPpmWindowsDiscarded;
    Counter rtlAutoPpmDecisionsApplied;
    Counter rtlAutoPpmDecisionsHeld;
    Counter rtlAutoPpmDecisionsFailed;
    std::atomic<int> rtlAutoPpmPhase { 0 }; // 0 disabled, 1 warmup, 2 measuring

    const char* rtlAutoPpmPhaseName() const {
        switch (rtlAutoPpmPhase.load(std::memory_order_relaxed)) {
        case 1: return "warmup";
        case 2: return "measuring";
        default: return "disabled";
        }
    }

    double rtlAutoPpmEstimateAge() const {
        const double last = rtlAutoPpmLastEstimateSteady.get();
        return last > 0.0 ? steadySeconds() - last : -1.0;
    }

    // ---- applied gain -----------------------------------------------------
    // overall is the combined control, lna/mixer/vga the stages. The unit and
    // the mode travel as small atomics of fixed values so the metrics thread
    // can label the series without sharing a std::string.
    Gauge deviceGainOverall;
    Gauge deviceGainLna;
    Gauge deviceGainMixer;
    Gauge deviceGainVga;
    Gauge deviceGainAuto;
    std::atomic<int> deviceGainUnit { 0 };       // 0 index, 1 db
    std::atomic<int> deviceGainMode { 0 };       // 0 none, 1 linearity, 2 sensitivity, 3 manual, 4 tuner, 5 auto
    std::atomic<bool> deviceGainHasStages { false };

    void setGainState(bool hasStages, bool db, int mode, bool autoGain, double overall, double lna,
                      double mixer, double vga) {
        if constexpr (!Enabled)
            return;
        deviceGainHasStages.store(hasStages, std::memory_order_relaxed);
        deviceGainUnit.store(db ? 1 : 0, std::memory_order_relaxed);
        deviceGainMode.store(mode, std::memory_order_relaxed);
        deviceGainAuto.set(autoGain ? 1.0 : 0.0);
        deviceGainOverall.set(overall);
        deviceGainLna.set(lna);
        deviceGainMixer.set(mixer);
        deviceGainVga.set(vga);
    }

    bool gainHasStages() const { return deviceGainHasStages.load(std::memory_order_relaxed); }

    const char* gainUnitName() const {
        return deviceGainUnit.load(std::memory_order_relaxed) == 1 ? "db" : "index";
    }

    const char* gainModeName() const {
        switch (deviceGainMode.load(std::memory_order_relaxed)) {
        case 1: return "linearity";
        case 2: return "sensitivity";
        case 3: return "manual";
        case 4: return "tuner";
        case 5: return "auto";
        default: return "none";
        }
    }

    // ---- signal quality and aircraft tracking -----------------------------
    // Per-frame observations written straight through atomics: a few hundred
    // events a second, not the sample loop. Signal/noise/SNR also cost a pass
    // over the retained ring, so they are only collected while someone is
    // actually scraping the endpoint.
    Histogram<8> rssiRatio;
    Histogram<25> signalDbfs;
    Histogram<25> noiseDbfs;
    Histogram<16> snrDb;
    Histogram<8> preambleScore;
    Gauge aircraftTracked;
    Gauge aircraftTrusted;
    std::atomic<bool> collectSignalQuality { false };

    void setSignalQualityCollection(bool on) noexcept {
        collectSignalQuality.store(on, std::memory_order_relaxed);
    }

    bool signalQualityCollection() const noexcept {
        return collectSignalQuality.load(std::memory_order_relaxed);
    }

    void observeSignalQuality(const SignalQuality& quality) noexcept {
        signalDbfs.observe(quality.signalDbfs, DbfsBounds);
        noiseDbfs.observe(quality.noiseDbfs, DbfsBounds);
        snrDb.observe(quality.snrDb, SnrBounds);
    }

    // ---- watchdog and sample drops ---------------------------------------
    Counter watchdogLate;
    Counter watchdogLost;
    Counter sampleDropEvents;
    Gauge sampleDropDeficit;
    Gauge sampleDropWorst;

    // ---- device recovery -------------------------------------------------
    Counter deviceLost;
    Counter deviceRecoveryAttempts;
    Counter deviceRecoveryFailed;

    // ---- demodulator (published from the demodulation thread) -------------
    std::array<std::atomic<uint64_t>, Stats::NUM_EVENTS> demodEvents {};
    std::array<std::atomic<uint64_t>, Stats::NumDF> demodSent {};
    std::array<std::atomic<uint64_t>, Stats::NumDF> demodDups {};
    std::array<std::atomic<uint64_t>, Stats::NUM_TC_GROUPS> demodTypeCodeGroups {};
    std::array<std::atomic<uint64_t>, Stats::NumControlFields> demodControlFields {};

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
        for (size_t i = 0; i < Stats::NUM_TC_GROUPS; i++)
            demodTypeCodeGroups[i].store(counters.typeCodeGroups[i], std::memory_order_relaxed);
        for (size_t i = 0; i < Stats::NumControlFields; i++)
            demodControlFields[i].store(counters.controlFields[i], std::memory_order_relaxed);
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
    Counter tcpRejectedClients;
    Counter tcpFramesSentAvr;
    Counter tcpFramesSentBeast;

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
    const std::string& commit() const { return m_commit; }
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
    std::string m_commit { "unknown" };
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
    "dup_phase_short",
    "dup_phase_long",
    "df17_repair_table_success",
    "df17_repair_erasure_success",
    "df17_repair_orbgrand_success",
    "df17_repair_rej_no_table_entry",
    "df17_repair_rej_untrusted",
    "df17_repair_rej_unsolved",
    "df17_repair_rej_weight",
    "df17_repair_rej_position",
} };
static_assert(EventNames.size() == Stats::NUM_EVENTS, "event name table out of sync with the Stats enum");

struct EventMetric {
    const char* metric;
    const char* label;
    const char* value;
    Stats::EventType event;
};

/// A small set of friendly aliases on top of the generic family, so the rules
/// file and dashboards do not have to spell the enum names.
inline constexpr std::array<EventMetric, 22> EventMetrics { {
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
    { "repairs_total", "method", "error_table", Stats::DF17_REPAIR_TABLE_SUCCESS },
    { "repairs_total", "method", "erasure", Stats::DF17_REPAIR_ERASURE_SUCCESS },
    { "repairs_total", "method", "orbgrand", Stats::DF17_REPAIR_ORBGRAND_SUCCESS },
    { "repairs_rejected_total", "reason", "no_table_entry", Stats::DF17_REPAIR_REJ_NO_TABLE_ENTRY },
    { "repairs_rejected_total", "reason", "untrusted_icao", Stats::DF17_REPAIR_REJ_UNTRUSTED },
    { "repairs_rejected_total", "reason", "erasure_unsolved", Stats::DF17_REPAIR_REJ_UNSOLVED },
    { "repairs_rejected_total", "reason", "weight_cap", Stats::DF17_REPAIR_REJ_WEIGHT },
    { "repairs_rejected_total", "reason", "position", Stats::DF17_REPAIR_REJ_POSITION },
    { "dedup_suppressed_total", "layer", "phase_short", Stats::DUP_PHASE_SHORT },
    { "dedup_suppressed_total", "layer", "phase_long", Stats::DUP_PHASE_LONG },
} };

struct MetricHelp {
    const char* metric;
    const char* help;
};

inline constexpr std::array<MetricHelp, 8> EventHelp { {
    { "es_frames_total", "Extended squitter frame positions by checksum outcome. Their sum is the trigger count." },
    { "df11_total", "All call replies by outcome." },
    { "frames_rejected_total", "Frames dropped by a field plausibility or signal gate." },
    { "frames_rescued_total", "Frames a held first sighting recovered after a plausibility gate rejected them." },
    { "repairs_total", "Damaged extended squitters recovered, by repair method." },
    { "repairs_rejected_total", "Repair attempts thrown away, by the check that rejected them." },
    { "dedup_suppressed_total", "Frames suppressed by a deduplication layer." },
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

/// Renders one histogram family in the Prometheus exposition format: cumulative
/// buckets (the last one +Inf), then the sum and the observation count.
template <size_t NumBuckets, size_t N>
inline void histogram(std::string& out, const char* name, const char* help, const Histogram<NumBuckets>& h,
                      const std::array<double, N>& bounds) {
    static_assert(N == NumBuckets, "one bucket per bound");
    head(out, name, help, "histogram");

    const std::string bucketName = std::string(name) + "_bucket";
    uint64_t cumulative = 0;
    for (size_t b = 0; b < N; ++b) {
        cumulative += h.buckets[b].load(std::memory_order_relaxed);
        char bound[32];
        std::snprintf(bound, sizeof(bound), "%.6g", bounds[b]);
        sample(out, bucketName.c_str(), labels({ { "le", bound } }), double(cumulative));
    }
    cumulative += h.buckets[N].load(std::memory_order_relaxed);
    sample(out, bucketName.c_str(), labels({ { "le", "+Inf" } }), double(cumulative));

    const std::string sumName = std::string(name) + "_sum";
    const std::string countName = std::string(name) + "_count";
    sample(out, sumName.c_str(), "", h.sum.load(std::memory_order_relaxed));
    sample(out, countName.c_str(), "", double(h.count.load(std::memory_order_relaxed)));
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
               { "commit", reg.commit() },
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
    sample(out, "device_setting", labels({ { "setting", "ppm" } }), reg.settingPpm.get());
    sample(out, "device_setting", labels({ { "setting", "frequency_hz" } }), reg.settingFrequencyHz.get());
    sample(out, "device_setting", labels({ { "setting", "agc" } }), reg.settingAgc.get());
    head(out, "device_gain",
         "Applied receiver gain, read back from the device shadow state rather than the config "
         "file. stage=overall is the combined control, lna/mixer/vga are the stages. unit is "
         "'index' for Airspy presets and the RTL stage registers, 'db' for the RTL tuner gain.",
         "gauge");
    sample(out, "device_gain", labels({ { "stage", "overall" }, { "unit", reg.gainUnitName() } }),
           reg.deviceGainOverall.get());
    if (reg.gainHasStages()) {
        sample(out, "device_gain", labels({ { "stage", "lna" }, { "unit", "index" } }),
               reg.deviceGainLna.get());
        sample(out, "device_gain", labels({ { "stage", "mixer" }, { "unit", "index" } }),
               reg.deviceGainMixer.get());
        sample(out, "device_gain", labels({ { "stage", "vga" }, { "unit", "index" } }),
               reg.deviceGainVga.get());
    }
    head(out, "device_gain_auto", "1 when the receiver is in automatic gain mode.", "gauge");
    sample(out, "device_gain_auto", "", reg.deviceGainAuto.get());
    head(out, "device_gain_mode", "Which gain control is in effect, always 1.", "gauge");
    sample(out, "device_gain_mode", labels({ { "mode", reg.gainModeName() } }), 1.0);
    head(out, "config_generation", "Increments on every accepted configuration reload.", "gauge");
    sample(out, "config_generation", "", reg.configGeneration.get());
    head(out, "config_reloads_total", "SIGHUP configuration reloads by outcome.", "counter");
    sample(out, "config_reloads_total", labels({ { "result", "ok" } }), double(reg.configReloadsOk.get()));
    sample(out, "config_reloads_total", labels({ { "result", "failed" } }), double(reg.configReloadsFailed.get()));

    if (reg.deviceName() == "rtlsdr") {
        head(out, "rtl_auto_ppm_enabled", "1 when RTL-SDR automatic crystal calibration is enabled.", "gauge");
        sample(out, "rtl_auto_ppm_enabled", "", reg.rtlAutoPpmEnabled.get());
        head(out, "rtl_auto_ppm_state", "Current automatic PPM controller phase, always 1.", "gauge");
        sample(out, "rtl_auto_ppm_state", labels({ { "state", reg.rtlAutoPpmPhaseName() } }), 1.0);
        head(out, "rtl_frequency_correction_ppm", "Frequency correction currently applied to librtlsdr.", "gauge");
        sample(out, "rtl_frequency_correction_ppm", "", reg.rtlAutoPpmCorrection.get());
        head(out, "rtl_auto_ppm_last_observation_ppm", "Residual clock error from the latest clean window.", "gauge");
        sample(out, "rtl_auto_ppm_last_observation_ppm", "", reg.rtlAutoPpmLastObservation.get());
        head(out, "rtl_auto_ppm_median_residual_ppm", "Median residual used by the latest controller decision.", "gauge");
        sample(out, "rtl_auto_ppm_median_residual_ppm", "", reg.rtlAutoPpmMedianResidual.get());
        head(out, "rtl_auto_ppm_estimated_error_ppm", "Estimated crystal error: applied correction plus median residual.", "gauge");
        sample(out, "rtl_auto_ppm_estimated_error_ppm", "", reg.rtlAutoPpmEstimatedError.get());
        head(out, "rtl_auto_ppm_sample_rate_hz", "Sample rate observed during the latest clean window.", "gauge");
        sample(out, "rtl_auto_ppm_sample_rate_hz", "", reg.rtlAutoPpmSampleRateHz.get());
        head(out, "rtl_auto_ppm_windows_collected", "Clean windows accumulated toward the next median.", "gauge");
        sample(out, "rtl_auto_ppm_windows_collected", "", reg.rtlAutoPpmWindowsCollected.get());
        head(out, "rtl_auto_ppm_window_seconds", "Duration of one clock measurement window.", "gauge");
        sample(out, "rtl_auto_ppm_window_seconds", "", reg.rtlAutoPpmWindowSeconds.get());
        head(out, "rtl_auto_ppm_target_windows", "Clean windows required for one median estimate.", "gauge");
        sample(out, "rtl_auto_ppm_target_windows", "", reg.rtlAutoPpmTargetWindows.get());
        head(out, "rtl_auto_ppm_warmup_seconds", "Warm-up delay before clock measurements begin.", "gauge");
        sample(out, "rtl_auto_ppm_warmup_seconds", "", reg.rtlAutoPpmWarmupSeconds.get());
        head(out, "rtl_auto_ppm_deadband_ppm", "Residual magnitude that does not trigger a correction.", "gauge");
        sample(out, "rtl_auto_ppm_deadband_ppm", "", reg.rtlAutoPpmDeadband.get());
        head(out, "rtl_auto_ppm_max_step_ppm", "Largest correction change allowed per decision.", "gauge");
        sample(out, "rtl_auto_ppm_max_step_ppm", "", reg.rtlAutoPpmMaxStep.get());
        head(out, "rtl_auto_ppm_limit_ppm", "Absolute correction safety limit.", "gauge");
        sample(out, "rtl_auto_ppm_limit_ppm", "", reg.rtlAutoPpmLimit.get());
        head(out, "rtl_auto_ppm_last_estimate_age_seconds", "Age of the latest median estimate; -1 before the first one.", "gauge");
        sample(out, "rtl_auto_ppm_last_estimate_age_seconds", "", reg.rtlAutoPpmEstimateAge());
        head(out, "rtl_auto_ppm_windows_total", "Measurement windows by outcome.", "counter");
        sample(out, "rtl_auto_ppm_windows_total", labels({ { "result", "clean" } }), double(reg.rtlAutoPpmWindowsClean.get()));
        sample(out, "rtl_auto_ppm_windows_total", labels({ { "result", "sample_drop" } }), double(reg.rtlAutoPpmWindowsDiscarded.get()));
        head(out, "rtl_auto_ppm_decisions_total", "Controller decisions by result.", "counter");
        sample(out, "rtl_auto_ppm_decisions_total", labels({ { "result", "applied" } }), double(reg.rtlAutoPpmDecisionsApplied.get()));
        sample(out, "rtl_auto_ppm_decisions_total", labels({ { "result", "held" } }), double(reg.rtlAutoPpmDecisionsHeld.get()));
        sample(out, "rtl_auto_ppm_decisions_total", labels({ { "result", "failed" } }), double(reg.rtlAutoPpmDecisionsFailed.get()));
    }

    head(out, "watchdog_events_total", "Watchdog observations about device liveness.", "counter");
    sample(out, "watchdog_events_total", labels({ { "event", "late" } }), double(reg.watchdogLate.get()));
    sample(out, "watchdog_events_total", labels({ { "event", "lost" } }), double(reg.watchdogLost.get()));

    head(out, "sample_drop_events_total", "Sample drop events reported by the device layer.", "counter");
    sample(out, "sample_drop_events_total", "", double(reg.sampleDropEvents.get()));
    head(out, "sample_drop_deficit_pairs", "Cumulative IQ pairs the device layer knows it lost.", "gauge");
    sample(out, "sample_drop_deficit_pairs", "", reg.sampleDropDeficit.get());
    head(out, "sample_drop_worst_deficit_pairs", "Worst single drop gap observed this run.", "gauge");
    sample(out, "sample_drop_worst_deficit_pairs", "", reg.sampleDropWorst.get());

    head(out, "device_lost_total", "Devices lost (no samples for over a second) detected by the watchdog.", "counter");
    sample(out, "device_lost_total", "", double(reg.deviceLost.get()));
    head(out, "device_recovery_attempts_total", "Device re-selection attempts after a loss.", "counter");
    sample(out, "device_recovery_attempts_total", "", double(reg.deviceRecoveryAttempts.get()));
    head(out, "device_recovery_failed_total", "Device losses that did not recover within the attempt budget.", "counter");
    sample(out, "device_recovery_failed_total", "", double(reg.deviceRecoveryFailed.get()));

    // ---- signal quality ---------------------------------------------------
    histogram(out, "message_rssi_ratio",
              "Normalised peak of accepted frames, 0..1 (the byte the AVR output carries divided by 255).",
              reg.rssiRatio, RssiRatioBounds);
    histogram(out, "signal_dbfs", "Per-frame signal level in dBFS, from the retained sample ring.",
              reg.signalDbfs, DbfsBounds);
    histogram(out, "noise_dbfs", "Local noise floor in dBFS at frame time.", reg.noiseDbfs, DbfsBounds);
    histogram(out, "snr_db", "Signal minus noise floor in dB for accepted frames.", reg.snrDb, SnrBounds);
    histogram(out, "preamble_score",
              "Weakest preamble pulse over strongest inter-pulse gap; a real preamble scores well "
              "above 1, noise hovers near 0.5.",
              reg.preambleScore, PreambleScoreBounds);

    // ---- aircraft table ---------------------------------------------------
    head(out, "aircraft_tracked", "Addresses currently alive in the table.", "gauge");
    sample(out, "aircraft_tracked", "", reg.aircraftTracked.get());
    head(out, "aircraft_trusted", "Addresses confirmed by an all call reply and still alive.", "gauge");
    sample(out, "aircraft_trusted", "", reg.aircraftTrusted.get());

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

    static constexpr std::array<const char*, Stats::NUM_TC_GROUPS> TypeCodeNames {
        "ident", "surface_position", "airborne_position", "velocity", "status", "other"
    };
    head(out, "es_messages_total", "Accepted extended squitters by type code group.", "counter");
    for (size_t g = 0; g < Stats::NUM_TC_GROUPS; g++)
        sample(out, "es_messages_total", labels({ { "tc_group", TypeCodeNames[g] } }),
               double(load(reg.demodTypeCodeGroups[g])));

    head(out, "df18_messages_total", "Accepted DF 18 frames by control field, separating real ADS-B from rebroadcast.",
         "counter");
    for (size_t cf = 0; cf < Stats::NumControlFields; cf++) {
        const auto v = load(reg.demodControlFields[cf]);
        if (v == 0)
            continue;
        sample(out, "df18_messages_total", labels({ { "cf", std::to_string(cf) } }), double(v));
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
    head(out, "tcp_rejected_clients_total",
         "Clients refused because the listener already had the maximum number connected.", "counter");
    sample(out, "tcp_rejected_clients_total", "", double(reg.tcpRejectedClients.get()));
    head(out, "tcp_frames_sent_total", "Frames encoded and handed to the TCP clients, by protocol.",
         "counter");
    sample(out, "tcp_frames_sent_total", labels({ { "protocol", "avr" } }), double(reg.tcpFramesSentAvr.get()));
    sample(out, "tcp_frames_sent_total", labels({ { "protocol", "beast" } }),
           double(reg.tcpFramesSentBeast.get()));

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
