/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2025 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#pragma once

#include "Bits128.hpp"
#include "CRC.hpp"
#include "Simd.hpp"

// Downlink formats the demodulator actually looks at: 0, 4, 5, 11, 16, 17, 18,
// 19, 20, 21. Every other value is dropped right away, so the shift register
// can tell the caller which streams are worth a closer look and which are not.
inline constexpr uint32_t handledDownlinkFormats =
      (1u <<  0) | (1u <<  4) | (1u <<  5) | (1u << 11)
    | (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19)
    | (1u << 20) | (1u << 21);

template<int NumStreams>
class alignas(16) ShiftRegistersBase {
    public:

    // the streams are grouped in pairs for the SIMD path
    static_assert(NumStreams % 2 == 0, "the number of streams is always even");

    // one bit per stream has to fit into the returned mask
    static_assert(NumStreams <= 64, "stream mask is 64 bit wide");

    constexpr ShiftRegistersBase() noexcept {
        for (auto i = 0; i < NumStreams; i++) {
            m_crc_56[i]  = 0;
            m_crc_112[i] = 0;
            m_high[i] = 0;
            m_low[i] = 0;
            m_df[i] = 0;
        };
    }

    constexpr CRC::crc_t getCRC_56(auto i) const noexcept {
        return (CRC::crc_t)m_crc_56[i];
    }

    constexpr CRC::crc_t getCRC_112(auto i) const noexcept {
        return (CRC::crc_t)m_crc_112[i];
    }

    constexpr uint32_t getDF(auto i) const noexcept{
        return (uint32_t)m_df[i];
    }

    protected:

    uint64_t m_low[NumStreams];
    uint64_t m_high[NumStreams];

	// And a checksum for the short messages (56 bit).
	// Kept in 64 bit lanes so the whole update runs in one SIMD width.
	uint64_t m_crc_56[NumStreams];

    // Each stream has a checksum for long messages (112 bit)
	uint64_t m_crc_112[NumStreams];

    uint64_t m_df[NumStreams];
};


template<int NumStreams>
class alignas(16) ShiftRegisters : public ShiftRegistersBase<NumStreams> {
    public:
        constexpr ShiftRegisters() : ShiftRegistersBase<NumStreams>() { }

        /// Shifts one new bit into every stream and updates both CRCs.
        /// Returns a mask with bit i set when stream i now shows a downlink
        /// format the demodulator handles.
        ///
        /// The two conditionals of the textbook version (did we shift a one out
        /// of the register, did the CRC overflow past 24 bits) are replaced by
        /// masks. Both are close to coin flips, so as branches they were mostly
        /// mispredictions, and without them the loop vectorizes.
        uint64_t shiftInNewBits(const uint32_t* cmp) noexcept {
#if STREAM1090_HAS_NEON64
            return shiftInNewBitsNeon(cmp);
#else
            return shiftInNewBitsScalar(cmp);
#endif
        }

        constexpr Bits128 extractAlignedFrameLong(auto i) const noexcept {
            return Bits128(this->m_high[i] >> 16, (this->m_low[i] >> 16) | (this->m_high[i] << 48));
        }

        constexpr uint64_t extractAlignedFrameShort(auto i) const noexcept {
            return (this->m_high[i] >> 8);
        }

    private:
        static constexpr uint64_t Delta55  = CRC::delta<55>();
        static constexpr uint64_t Delta111 = CRC::delta<111>();
        static constexpr uint64_t Polynomial = CRC::polynomial;

        uint64_t shiftInNewBitsScalar(const uint32_t* cmp) noexcept {
            uint64_t handledMask = 0;

            for (auto i = 0; i < NumStreams; i++) {
                const uint64_t high = this->m_high[i];
                const uint64_t low  = this->m_low[i];

                // all ones when a one bit leaves the register at the top
                const uint64_t shiftedOut = uint64_t(0) - (high >> 63);

                uint64_t crc56  = this->m_crc_56[i]  ^ (Delta55  & shiftedOut);
                uint64_t crc112 = this->m_crc_112[i] ^ (Delta111 & shiftedOut);

                crc56  = (crc56  << 1) | ((high >> 7)  & 0x1);
                crc112 = (crc112 << 1) | ((low  >> 15) & 0x1);

                const uint64_t newHigh = (high << 1) | (low >> 63);
                const uint64_t newLow  = (low  << 1) | cmp[i];
                const uint64_t df      = newHigh >> 59;

                // fold the polynomial back in whenever bit 24 is set
                crc56  ^= Polynomial & (uint64_t(0) - (crc56  >> 24));
                crc112 ^= Polynomial & (uint64_t(0) - (crc112 >> 24));

                this->m_crc_56[i]  = crc56;
                this->m_crc_112[i] = crc112;
                this->m_high[i]    = newHigh;
                this->m_low[i]     = newLow;
                this->m_df[i]      = df;

                handledMask |= uint64_t((handledDownlinkFormats >> df) & 0x1) << i;
            }

            return handledMask;
        }

#if STREAM1090_HAS_NEON64
        /// Same update, two streams per vector. The streams are completely
        /// independent of each other, so this is a plain width increase.
        uint64_t shiftInNewBitsNeon(const uint32_t* cmp) noexcept {
            const uint64x2_t delta55   = vdupq_n_u64(Delta55);
            const uint64x2_t delta111  = vdupq_n_u64(Delta111);
            const uint64x2_t polynomial = vdupq_n_u64(Polynomial);
            const uint64x2_t one       = vdupq_n_u64(1);
            const uint64x2_t handled   = vdupq_n_u64(handledDownlinkFormats);

            uint64_t handledMask = 0;

            for (int i = 0; i < NumStreams; i += 2) {
                const uint64x2_t high = vld1q_u64(&this->m_high[i]);
                const uint64x2_t low  = vld1q_u64(&this->m_low[i]);

                // arithmetic shift by 63 smears the top bit over the whole lane
                const uint64x2_t shiftedOut = broadcastTopBit(high);

                uint64x2_t crc56  = veorq_u64(vld1q_u64(&this->m_crc_56[i]),
                                              vandq_u64(delta55, shiftedOut));
                uint64x2_t crc112 = veorq_u64(vld1q_u64(&this->m_crc_112[i]),
                                              vandq_u64(delta111, shiftedOut));

                crc56  = vorrq_u64(vshlq_n_u64(crc56, 1),
                                   vandq_u64(vshrq_n_u64(high, 7), one));
                crc112 = vorrq_u64(vshlq_n_u64(crc112, 1),
                                   vandq_u64(vshrq_n_u64(low, 15), one));

                const uint64x2_t newHigh = vorrq_u64(vshlq_n_u64(high, 1), vshrq_n_u64(low, 63));

                // the new bits arrive as two 32 bit values
                uint64x2_t bits = vsetq_lane_u64(cmp[i], vdupq_n_u64(0), 0);
                bits = vsetq_lane_u64(cmp[i + 1], bits, 1);
                const uint64x2_t newLow = vorrq_u64(vshlq_n_u64(low, 1), bits);

                // bit 24 set means the CRC needs the polynomial folded back in
                crc56  = veorq_u64(crc56,  vandq_u64(polynomial, broadcastTopBit(vshlq_n_u64(crc56, 39))));
                crc112 = veorq_u64(crc112, vandq_u64(polynomial, broadcastTopBit(vshlq_n_u64(crc112, 39))));

                const uint64x2_t df = vshrq_n_u64(newHigh, 59);

                vst1q_u64(&this->m_crc_56[i],  crc56);
                vst1q_u64(&this->m_crc_112[i], crc112);
                vst1q_u64(&this->m_high[i],    newHigh);
                vst1q_u64(&this->m_low[i],     newLow);
                vst1q_u64(&this->m_df[i],      df);

                // (handledDownlinkFormats >> df) & 1, a right shift is a
                // negative left shift on NEON
                const uint64x2_t isHandled =
                    vandq_u64(vshlq_u64(handled, vnegq_s64(vreinterpretq_s64_u64(df))), one);

                handledMask |= vgetq_lane_u64(isHandled, 0) << i;
                handledMask |= vgetq_lane_u64(isHandled, 1) << (i + 1);
            }

            return handledMask;
        }

        // all ones in a lane whose top bit is set, all zeros otherwise
        static inline uint64x2_t broadcastTopBit(uint64x2_t v) noexcept {
            return vreinterpretq_u64_s64(vshrq_n_s64(vreinterpretq_s64_u64(v), 63));
        }
#endif
};
