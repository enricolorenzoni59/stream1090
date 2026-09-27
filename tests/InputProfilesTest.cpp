/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "RateUtils.hpp"
#include "Cli.hpp"
#include "devices/SdrplaySettings.hpp"
#include <stdexcept>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(0)
int main() {
    CHECK(is_valid_rate_pair(Rate_4_0_Mhz, Rate_8_0_Mhz, InputFormatType::IQ_INT16_FULL_SCALE));
    CHECK(!is_valid_rate_pair(Rate_4_0_Mhz, Rate_8_0_Mhz, InputFormatType::IQ_UINT8_RTL_SDR));
    CHECK(!is_valid_rate_pair(Rate_2_4_Mhz, Rate_12_0_Mhz, InputFormatType::IQ_INT16_FULL_SCALE));
    CHECK(is_valid_rate_pair(Rate_2_4_Mhz, Rate_12_0_Mhz, InputFormatType::IQ_UINT8_RTL_SDR));
    CHECK(find_default_output_rate(Rate_6_0_Mhz,InputFormatType::IQ_INT16_FULL_SCALE)==Rate_12_0_Mhz);
    CHECK(find_default_output_rate(Rate_6_0_Mhz,InputFormatType::IQ_UINT16_RAW_AIRSPY)==Rate_24_0_Mhz);
    for(int v=-32768;v<=32767;++v) CHECK(IQ_INT16_FULL_SCALE::convertFixed(int16_t(v))==v/2);
    CHECK(IQ_INT16_ANTSDR::convertFixed(1024)!=IQ_INT16_FULL_SCALE::convertFixed(1024));
    for(auto rate : {2000000u,2400000u,3000000u,4000000u,6000000u,7000000u,8000000u,9000000u,10000000u}) {
        SdrplaySettings{}.validate(rate,1090000000);
        auto taps=basebandTaps(static_cast<SampleRate>(rate));
        double sum=0; for(auto t:taps) sum+=t;
        CHECK(std::abs(sum-1)<1e-6);
        CHECK(FirDetail::tapsFitAccumulator(taps));
    }
    CHECK(SdrplaySettings::nominalAdcBits(4000000)==14);
    CHECK(SdrplaySettings::nominalAdcBits(7000000)==12);
    CHECK(SdrplaySettings::nominalAdcBits(9000000)==10);
    CHECK(SdrplaySettings::nominalAdcBits(10000000)==8);
    float f=0; CHECK(!parse_number("nan",f)); CHECK(!parse_number("inf",f)); CHECK(!parse_number("12oops",f));
    uint32_t n=0; CHECK(!parse_number("-1",n)); CHECK(!parse_number("4294967296",n));
    CHECK(parse_number("1090000000",n) && n==1090000000);
    for(int invalid:{19,60}) { auto cfg=SdrplaySettings{}; cfg.gainReduction=invalid;
        bool rejected=false; try{cfg.validate(4000000,1090000000);}catch(...){rejected=true;} CHECK(rejected); }
}
