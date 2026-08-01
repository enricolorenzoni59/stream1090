/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#pragma once

// Central place that decides whether the hand written SIMD kernels are used.
// Everything guarded by this has a plain C++ fallback that produces the same
// numbers, so switching it off only costs speed.

#if !defined(STREAM1090_SIMD)
#define STREAM1090_SIMD 1
#endif

#if STREAM1090_SIMD && defined(__ARM_NEON)
    #include <arm_neon.h>
    #define STREAM1090_HAS_NEON 1
#else
    #define STREAM1090_HAS_NEON 0
#endif

// AArch64 has the 64 bit lane compares and the horizontal reductions that the
// shift register kernel needs. 32 bit NEON does not, so it stays on the
// scalar path there.
#if STREAM1090_HAS_NEON && defined(__aarch64__)
    #define STREAM1090_HAS_NEON64 1
#else
    #define STREAM1090_HAS_NEON64 0
#endif

#if STREAM1090_SIMD && (defined(__SSE2__) || defined(_M_X64))
    #include <emmintrin.h>
    #define STREAM1090_HAS_SSE2 1
#else
    #define STREAM1090_HAS_SSE2 0
#endif
