#pragma once

#include "ModeS.hpp"

namespace Plausibility {
    
    // this function does a rough range check on a 24 bit icao hex
    inline bool checkICAO(const uint32_t& icao) {
        /* Coarse ranges. May produce false positives, but that is fine.
           These have been obtain from suspicious.py
                004000 - 0DFFFF
                100000 - 20FFFF
                300000 - 4DFFFF
                500000 - 51FFFF
                600000 - 60FFFF
                680000 - 68FFFF
                700000 - 8AFFFF
                900000 - 90FFFF
                A00000 - AFFFFF
                C00000 - C3FFFF
                C80000 - C9FFFF
                E00000 - E9FFFF */
        uint32_t first_4_bit = icao >> 20;

        switch (first_4_bit) {
            case 0x0 : return (0x004000 <= icao) && (icao <= 0x0DFFFF);
            case 0x1 : return true;
            case 0x2 : return (icao <= 0x20FFFF);
            case 0x3 : return true;
            case 0x4 : return (icao <= 0x4DFFFF);
            case 0x5 : return (icao <= 0x51FFFF);
            case 0x6 : return (icao <= 0x60FFFF) || ((0x680000 <= icao) && (icao <= 0x68FFFF));
            case 0x7 : return true;
            case 0x8 : return (icao <= 0x8AFFFF);
            case 0x9 : return (icao <= 0x90FFFF);
            case 0xA : return true;
            case 0xC : return (icao <= 0xC3FFFF) || ((0xC80000 <= icao) && (icao <= 0xC9FFFF));
            case 0xE : return (icao <= 0xE9FFFF);
            default:
                return false; 
        }
    }
    
    // checks basic properties of a DF-17 frame
    inline bool checkDF17(const Bits128& frame) {
        // transponder capability check. 1,2,3 do not have ADS-B.
        auto ca = ModeS::getField<ModeS::DF17::CA>(frame);
        if ((1 <= ca) && (ca <= 3))
            return false;

        // message typecode 23 ... 27 are not allowed
        auto tc = ModeS::getField<ModeS::DF17::Typecode>(frame);
        if ((23 <= tc) && (tc <= 27))
            return false;

        return true;
    }
}