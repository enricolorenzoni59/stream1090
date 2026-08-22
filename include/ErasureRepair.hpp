/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Syndrome-based erasure decoding for the Mode-S CRC-24.
 *
 * A frame's syndrome is the XOR of the per-bit syndrome deltas of every bit
 * that got flipped. So if we can name a small set of candidate error positions
 * -- the bits the demodulator was least confident about -- recovering the error
 * pattern becomes a linear system over GF(2) with 24 equations:
 *
 *     XOR_{i in S} delta[p_i] = syndrome
 *
 * where S is the subset of candidates that were actually wrong. Solving it by
 * Gaussian elimination generalises the fixed syndrome tables in CRCErrorTable.hpp
 * from a catalogue of specific burst shapes to arbitrary patterns.
 *
 * Safety
 * ------
 * A random syndrome is spuriously "solvable" with probability 2^(k-24), where k
 * is the number of CANDIDATES offered -- note, not the number of errors. That
 * alone would cap k around 16.
 *
 * But a genuine error pattern is a handful of bits, while a spurious solve over
 * k candidates flips about k/2 of them. Rejecting solutions heavier than
 * MaxWeight therefore shrinks the accepted set from 2^k to sum_{w<=W} C(k,w),
 * and the bound becomes
 *
 *     sum_{w <= MaxWeight} C(Candidates, w) / 2^24
 *
 * which buys enough budget to widen the window instead. That matters because
 * the bits an interfering signal corrupts are not always the ones the
 * demodulator was least sure about, so a wider window catches damage a tight
 * one ranks out of reach.
 *
 * For reference DF17ErrorTableExperimental holds ~933 syndromes, a false-hit
 * rate of 5.6e-5. Points measured against a 60 s Airspy capture:
 *
 *     k=16 W=inf   3.9e-3   +491 messages
 *     k=24 W=5     3.3e-3   +585 messages   <- chosen: more repairs, lower bound
 *     k=24 W=6     1.1e-2   +624 messages, and the first false repairs appear
 *
 * Acceptance is otherwise unchanged from the table path: a repair is only
 * emitted if the recovered address belongs to an aircraft already trusted from
 * DF11.
 */
#pragma once

#include "CRC.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ErasureRepair {

inline constexpr size_t MaxFrameBits = 112;

/// Number of least-confident bit positions offered to the solver.
inline constexpr int Candidates = 24;

/// Reject any solution flipping more than this many bits. This is what bounds
/// false repairs -- see the safety note above before changing either constant.
inline constexpr int MaxWeight = 5;

static_assert(Candidates > 0 && Candidates <= 32, "Candidates must fit the 32-bit solution mask");
static_assert(MaxWeight > 0 && MaxWeight < Candidates, "MaxWeight must actually constrain the solution");

/// delta[i] is the syndrome contribution of flipping frame bit i. Same
/// recurrence as CRC::delta<i>(), but as a table so the index can be dynamic.
inline const std::array<CRC::crc_t, MaxFrameBits>& deltaTable() {
    static const auto table = [] {
        std::array<CRC::crc_t, MaxFrameBits> t{};
        CRC::crc_t crc = 0x1;
        for (size_t i = 0; i < MaxFrameBits; ++i) {
            t[i] = crc;
            crc <<= 1;
            if (crc & (0x1u << 24))
                crc ^= CRC::polynomial;
        }
        return t;
    }();
    return table;
}

struct Result {
    bool solved = false;
    uint32_t mask = 0; ///< bit i set => candidates[i] was flipped
};

/// Solve for which of @p candidates were flipped, given the frame's syndrome.
///
/// Gauss-Jordan over GF(2): each candidate contributes a 24-bit column, we
/// reduce those into an echelon basis carrying the combination that produced
/// each pivot, then reduce the syndrome against that basis. If it reduces to
/// zero the accumulated combination is an error pattern explaining it.
inline Result solve(CRC::crc_t syndrome, const uint8_t* candidates, size_t count) {
    Result result;
    if (count == 0 || count > 32)
        return result;

    const auto& delta = deltaTable();

    CRC::crc_t pivotValue[24] = {};
    uint32_t pivotMask[24] = {};
    bool pivotUsed[24] = {};

    for (size_t i = 0; i < count; ++i) {
        if (candidates[i] >= MaxFrameBits)
            return result;

        CRC::crc_t value = delta[candidates[i]];
        uint32_t mask = (uint32_t(1) << i);

        for (int bit = 23; bit >= 0; --bit) {
            if (!((value >> bit) & 1))
                continue;
            if (!pivotUsed[bit]) {
                pivotUsed[bit] = true;
                pivotValue[bit] = value;
                pivotMask[bit] = mask;
                value = 0;
                break;
            }
            value ^= pivotValue[bit];
            mask ^= pivotMask[bit];
        }
        // value == 0 here means the column was dependent on earlier ones
    }

    CRC::crc_t residual = syndrome;
    uint32_t combination = 0;
    for (int bit = 23; bit >= 0; --bit) {
        if (!((residual >> bit) & 1))
            continue;
        if (!pivotUsed[bit])
            return result; // syndrome lies outside the span
        residual ^= pivotValue[bit];
        combination ^= pivotMask[bit];
    }

    if (residual != 0)
        return result;

    result.solved = true;
    result.mask = combination;
    return result;
}

} // namespace ErasureRepair

/*
 * ORBGRAND enumerates error patterns over all searchable positions in
 * reliability order. The hard guess budget bounds the chance of matching a
 * random 24-bit syndrome to MaxGuesses / 2^24.
 */
namespace OrbGrand {

#ifndef STREAM1090_ORBGRAND_GUESSES
#define STREAM1090_ORBGRAND_GUESSES 1024
#endif
inline constexpr int MaxGuesses = STREAM1090_ORBGRAND_GUESSES;

inline constexpr int MaxFlips = 6;

static_assert(MaxGuesses > 0, "ORBGRAND needs a positive guess budget");

struct Result {
    bool solved = false;
    int count = 0;
    uint8_t flips[MaxFlips] = {};
    int guesses = 0;
};

namespace detail {

struct Search {
    CRC::crc_t syndrome;
    const CRC::crc_t* delta;
    const uint8_t* position;
    int guesses = 0;
    int depth = 0;
    uint8_t ranks[MaxFlips] = {};
    Result* out = nullptr;

    // Generate sets of distinct ranks summing to remaining. Trying lower
    // logistic weights first approximates maximum-likelihood error ordering.
    void recurse(int remaining, int largest, CRC::crc_t accumulated) {
        if (out->solved || guesses >= MaxGuesses)
            return;
        if (remaining == 0) {
            ++guesses;
            if (accumulated == syndrome) {
                out->solved = true;
                out->count = depth;
                for (int i = 0; i < depth; ++i)
                    out->flips[i] = position[ranks[i] - 1];
            }
            return;
        }
        if (depth == MaxFlips)
            return;

        for (int part = (remaining < largest) ? remaining : largest; part >= 1; --part) {
            int reachable = 0;
            for (int i = 1; i <= MaxFlips - depth - 1; ++i) {
                const int next = part - i;
                if (next <= 0)
                    break;
                reachable += next;
            }
            if (remaining - part > reachable)
                break;

            ranks[depth++] = uint8_t(part);
            recurse(remaining - part, part - 1, accumulated ^ delta[part - 1]);
            --depth;
            if (out->solved || guesses >= MaxGuesses)
                return;
        }
    }
};

} // namespace detail

/// Decode a syndrome using frame bit positions ordered least reliable first.
inline Result decode(CRC::crc_t syndrome, const uint8_t* position, int numRanks) {
    Result result;
    if (syndrome == 0 || numRanks <= 0)
        return result;

    const auto& table = ErasureRepair::deltaTable();
    CRC::crc_t delta[ErasureRepair::MaxFrameBits];
    const int n = (numRanks > int(ErasureRepair::MaxFrameBits)) ? int(ErasureRepair::MaxFrameBits) : numRanks;
    for (int rank = 0; rank < n; ++rank) {
        if (position[rank] >= ErasureRepair::MaxFrameBits)
            return result;
        delta[rank] = table[position[rank]];
    }

    detail::Search search{syndrome, delta, position};
    search.out = &result;
    const int maxWeight = n * MaxFlips;
    for (int weight = 1; weight <= maxWeight; ++weight) {
        search.recurse(weight, n, 0);
        if (result.solved || search.guesses >= MaxGuesses)
            break;
    }
    result.guesses = search.guesses;
    return result;
}

} // namespace OrbGrand
