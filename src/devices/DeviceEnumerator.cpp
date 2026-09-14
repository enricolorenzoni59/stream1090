/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#include "devices/DeviceEnumerator.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef STREAM1090_HAVE_RTLSDR
#include <rtl-sdr.h>
#endif

#ifdef STREAM1090_HAVE_AIRSPY
#include <airspy.h>
#endif

std::vector<DeviceDescriptor> enumerateDevices() {
    std::vector<DeviceDescriptor> devices;

#ifdef STREAM1090_HAVE_AIRSPY
    {
        uint64_t serials[64] = {};
        const int count = airspy_list_devices(serials, 64);
        for (int i = 0; i < count; ++i) {
            char serial[32] = {};
            std::snprintf(serial, sizeof(serial), "%016llX", static_cast<unsigned long long>(serials[i]));
            devices.push_back({InputDeviceType::AIRSPY, serial, i});
        }
    }
#endif

#ifdef STREAM1090_HAVE_RTLSDR
    {
        const int count = rtlsdr_get_device_count();
        for (int i = 0; i < count; ++i) {
            char serial[256] = {};
            rtlsdr_get_device_usb_strings(i, nullptr, nullptr, serial);
            devices.push_back({InputDeviceType::RTLSDR, serial, i});
        }
    }
#endif

    return orderDevices(std::move(devices));
}

std::vector<uint32_t> queryAirspyRates(const std::string& serial) {
    std::vector<uint32_t> rates;
#ifdef STREAM1090_HAVE_AIRSPY
    airspy_device* dev = nullptr;
    uint64_t sn = 0;
    try {
        std::size_t pos = 0;
        sn = std::stoull(serial, &pos, 16);
        if (pos != serial.size())
            sn = 0;
    } catch (...) {
        sn = 0;
    }

    const int rc = (sn == 0) ? airspy_open(&dev) : airspy_open_sn(&dev, sn);
    if (rc != AIRSPY_SUCCESS || dev == nullptr)
        return rates;

    // No sample type is set on this fresh handle, so the rates come back at
    // their real value rather than doubled as they do for a REAL sample type.
    uint32_t count = 0;
    if (airspy_get_samplerates(dev, &count, 0) == AIRSPY_SUCCESS && count > 0) {
        rates.resize(count);
        if (airspy_get_samplerates(dev, rates.data(), count) != AIRSPY_SUCCESS)
            rates.clear();
    }
    airspy_close(dev);
#else
    (void)serial;
#endif
    return rates;
}
