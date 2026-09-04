/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "BitUtils.hpp"


// ########### ATTENTION #############
// This file defines the bit fields of long and short messages. 
// The indices used follow the ones used by ICAO and the 1090 Mhz riddle book.
// This means that for example that the 5 bits for the downlink format 
// indexed by 1 ... 5. With bit 1 being the MSB. This is just for convenience.
// ########### ATTENTION #############
namespace ModeS {

    // Index adjusted BitRange that also carries the type of the field for 56 bit frames
    template<typename T, std::size_t icao_lo, std::size_t icao_hi>
    struct FrameField_Short : BitUtils::BitRange<(56 - icao_lo), (56 - icao_hi)> {
        static_assert(icao_lo <= icao_hi);
        using Type = T;
        static constexpr std::size_t MessageLength = 56;
    };

    // Index adjusted BitRange that also carries the type of the field for 112 bit frames
    template<typename T, std::size_t icao_lo, std::size_t icao_hi>
    struct FrameField_Long : BitUtils::BitRange<(112 - icao_lo), (112 - icao_hi)> {
        static_assert(icao_lo <= icao_hi);
        using Type = T;
        static constexpr std::size_t MessageLength = 112;
    };

    template<typename Field>
    constexpr typename Field::Type getField(const uint64_t& bits) noexcept {
        static_assert(Field::MessageLength == 56);
        return BitUtils::getBits<typename Field::Type, Field::HI, Field::LO>(bits);
    }

    template<typename Field>
    constexpr typename Field::Type getField(const Bits128& bits) noexcept {
        static_assert(Field::MessageLength == 112);
        return BitUtils::getBits<typename Field::Type, Field::HI, Field::LO>(bits);
    }

    // DF 0: ACAS short
    namespace DF0 {
        using Vertical      = FrameField_Short< uint8_t,  6,  6>;
        using Crosslink     = FrameField_Short< uint8_t,  7,  7>;
        using Sensitivity   = FrameField_Short< uint8_t,  9, 11>;
        using Reply         = FrameField_Short< uint8_t, 14, 17>;
        using Altitude      = FrameField_Short<uint16_t, 20, 32>;
    } 

    // DF 4 and 5
    namespace Surv {
        using FlightStatus  = FrameField_Short< uint8_t,  6,  8>;
        using DownlinkReq   = FrameField_Short< uint8_t,  9, 13>;
        using UtilityMsg    = FrameField_Short< uint8_t, 14, 19>;
        using SquawkAlt     = FrameField_Short<uint16_t, 20, 32>;
        using Altitude      = SquawkAlt;
        using Squawk        = SquawkAlt;
    }

    // DF-11: All call reply
    namespace DF11 {
        using CA            = FrameField_Short<uint8_t,  6,  8>;
        using ICAO          = FrameField_Short<uint32_t, 9, 32>;
        using ICAOWithCA    = FrameField_Short<uint32_t, 6, 32>;
    }

    // ACAS Long 
    namespace DF16 {
        using Vertical      = FrameField_Long< uint8_t,  6,  6>;
        using Sensitivity   = FrameField_Long< uint8_t,  9, 11>;
        using Reply         = FrameField_Long< uint8_t, 14, 17>;
        using Altitude      = FrameField_Long<uint16_t, 20, 32>;
        using Message       = FrameField_Long<uint64_t, 33, 88>;
    }

    // ADS-B extended squitter
    namespace DF17 {
        using CA            = FrameField_Long< uint8_t,  6,  8>;
        using ICAO          = FrameField_Long<uint32_t,  9, 32>;
        using ICAOWithCA    = FrameField_Long<uint32_t,  6, 32>;
        using Message       = FrameField_Long<uint32_t, 33, 88>;
        using Typecode      = FrameField_Long< uint8_t, 33, 37>;

        // Airborne position
        using SurvStatus    = FrameField_Long< uint8_t, 38, 39>;
        using Antenna       = FrameField_Long< uint8_t, 40, 40>;
        using Altitude      = FrameField_Long<uint16_t, 41, 52>;
        using Time          = FrameField_Long< uint8_t, 53, 53>;
        using CPR_Odd       = FrameField_Long< uint8_t, 54, 54>;
        using CPR_Lat       = FrameField_Long<uint32_t, 55, 71>;
        using CPR_Lon       = FrameField_Long<uint32_t, 72, 88>;
    }

    // DF 20 and 21
    namespace CommB {
        using FlightStatus  = FrameField_Long< uint8_t,  6,  8>;
        using DownlinkReq   = FrameField_Long< uint8_t,  9, 13>;
        using UtilityMsg    = FrameField_Long< uint8_t, 14, 19>;
        using SquawkAlt     = FrameField_Long<uint16_t, 20, 32>;
        using Altitude      = SquawkAlt;
        using Squawk        = SquawkAlt;
    }

} // end of namespace ModeS