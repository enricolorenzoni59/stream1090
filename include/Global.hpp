/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include <atomic>
#include <csignal>

// visible to every translation unit that instantiates a sampler
#ifndef STREAM1090_VERSION
#define STREAM1090_VERSION "260905"
#endif

#ifndef STREAM1090_GIT_COMMIT
#define STREAM1090_GIT_COMMIT "unknown"
#endif

struct GlobalOptions {
#ifdef STATS_ENABLED
    static constexpr bool StatsEnabled = (STATS_ENABLED != 0);
#else
    static constexpr bool StatsEnabled = false;
#endif

#ifdef STATS_END_ONLY
    static constexpr bool StatsAtTheEndOnly = StatsEnabled && (STATS_END_ONLY != 0);
#else
    static constexpr bool StatsAtTheEndOnly = false;
#endif

#ifdef STREAM1090_CUSTOM_INPUT
    static constexpr bool CustomInputMode = (STREAM1090_CUSTOM_INPUT != 0);
#else
    static constexpr bool CustomInputMode = false;
#endif

#ifdef STREAM1090_RSSI
    static constexpr bool RSSIEnabled = (STREAM1090_RSSI != 0);
#else
    static constexpr bool RSSIEnabled = false;
#endif

#ifdef STREAM1090_HAVE_RTLSDR
    static constexpr bool NativeRtlSdrSupport = (STREAM1090_HAVE_RTLSDR != 0);
#else
    static constexpr bool NativeRtlSdrSupport = false;
#endif

#ifdef STREAM1090_HAVE_AIRSPY
    static constexpr bool NativeAirspySupport = (STREAM1090_HAVE_AIRSPY != 0);
#else
    static constexpr bool NativeAirspySupport = false;
#endif

#ifdef STREAM1090_HAVE_RTLSDR_BLOG
    static constexpr bool RtlSdrBlogAdvanced = (STREAM1090_HAVE_RTLSDR_BLOG != 0);
#else
    static constexpr bool RtlSdrBlogAdvanced = false;
#endif
};

namespace ProcessSignals {

// inline, not static: these are read and written from several translation
// units (the handler is installed in main.cpp, the pipeline and the watchdog
// check it from the preset translation units). A header-local `static` would
// give every TU its own copy, so a delivered SIGINT would set one copy and the
// reader would wait forever on another.
inline std::atomic<bool> g_shutdownRequested{false};
inline std::atomic<bool> g_reselectRequested{false};

inline bool shutdownRequested() {
    return g_shutdownRequested.load(std::memory_order_relaxed);
}

inline bool reselectRequested() {
    return g_reselectRequested.load(std::memory_order_relaxed);
}

inline void clearReselect() {
    g_reselectRequested.store(false, std::memory_order_relaxed);
}

inline void clearShutdown() {
    g_shutdownRequested.store(false, std::memory_order_relaxed);
}

inline void handle_sigint(int) {
    g_shutdownRequested.store(true, std::memory_order_relaxed);
}

// A SIGHUP no longer re-reads a file: it asks the supervisor to tear the
// current run down and select the device again, so a newly plugged dongle is
// picked up. Stopping the run is what wakes the DSP pipeline.
inline void handle_sighup(int) {
    g_reselectRequested.store(true, std::memory_order_relaxed);
    g_shutdownRequested.store(true, std::memory_order_relaxed);
}

inline void install() {
    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);
    std::signal(SIGHUP, handle_sighup);
}
} // end of namespace ProcessSignals
