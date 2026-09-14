/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */
#pragma once

#include "devices/DeviceConfig.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// One attached native device, described without opening it.
struct DeviceDescriptor {
    InputDeviceType type = InputDeviceType::NONE;
    std::string serial;
    int index = 0;
};

// Airspy before RTL-SDR; within one type ordered by serial string so the
// choice is stable across enumerations. Pure, so the ordering is unit tested
// without hardware.
inline std::vector<DeviceDescriptor> orderDevices(std::vector<DeviceDescriptor> devices) {
    std::stable_sort(devices.begin(), devices.end(), [](const DeviceDescriptor& a, const DeviceDescriptor& b) {
        if (a.type != b.type)
            return a.type == InputDeviceType::AIRSPY;
        return a.serial < b.serial;
    });
    return devices;
}

// Lists the attached devices without opening them. Empty when no backend is
// compiled in or nothing is plugged in.
std::vector<DeviceDescriptor> enumerateDevices();

// Sample rates the Airspy reports, in Hz. Opens the device briefly and closes
// it again; empty when the backend is absent or the device cannot be opened.
std::vector<uint32_t> queryAirspyRates(const std::string& serial);
