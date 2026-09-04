/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include <atomic>

// Process-wide decoded-message counters, bumped by the message handlers
// and read by the periodic RTL-SDR diagnostics.
namespace StreamCounters {
    inline std::atomic<uint64_t> decodedShort{0}; // 56-bit frames
    inline std::atomic<uint64_t> decodedLong{0};  // 112-bit frames
}
