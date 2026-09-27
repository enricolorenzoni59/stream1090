/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string>
#include <stdexcept>
#include <cstdint>

// Independent of both the vendor SDK and the application's CLI.
struct SdrplaySettings {
    int gainReduction = 40; // IF gain reduction in dB, not tuner gain
    int lnaState = 2;       // RF attenuation index, not dB
    int bandwidthKhz = 5000;
    int adsbMode = 1;       // no-decimation lowpass for zero IF; explicit for experiments
    bool rfNotch = false;
    bool dabNotch = false;
    bool usbBulk = false;  // Explicit alternative to the API's isochronous default
    static bool supportsRate(uint32_t hz) {
        switch (hz) {
        case 2000000: case 2400000: case 3000000: case 4000000: case 6000000:
        case 7000000: case 8000000: case 9000000: case 10000000: return true;
        default: return false;
        }
    }
    // Datasheet nominal ADC modes; API sample containers remain signed 16 bit.
    static int nominalAdcBits(uint32_t hz) {
        return hz < 6048000 ? 14 : hz < 8064000 ? 12 : hz < 9216000 ? 10 : 8;
    }
    void validate(uint32_t hz, uint32_t frequency) const {
        if (!supportsRate(hz)) throw std::invalid_argument("Unsupported RSP1B sample rate");
        // Keep this first backend's RF gain table and profile explicitly ADS-B.
        if (frequency != 1090000000) throw std::invalid_argument("RSP1B currently supports 1090 MHz only");
        if (gainReduction < 20 || gainReduction > 59)
            throw std::invalid_argument("--sdrplay-if-gr must be 20..59 dB");
        if (lnaState < 0 || lnaState > 8)
            throw std::invalid_argument("--sdrplay-lna-state must be 0..8 at 1090 MHz");
        if (adsbMode < 0 || adsbMode > 3)
            throw std::invalid_argument("--sdrplay-adsb-mode must be 0..3");
        switch (bandwidthKhz) {
        case 200: case 300: case 600: case 1536: case 5000: case 6000: case 7000: case 8000: break;
        default: throw std::invalid_argument("Invalid --sdrplay-bandwidth (kHz)");
        }
    }
};
