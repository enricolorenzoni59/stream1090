/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "devices/DeviceEnumerator.hpp"

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        failures++;
    }
}

} // namespace

int main() {
    // Input deliberately shuffled: an RTL-SDR before any Airspy, and serials
    // out of order within each type.
    std::vector<DeviceDescriptor> input = {
        {InputDeviceType::RTLSDR, "00000002", 2},
        {InputDeviceType::AIRSPY, "00000000000000AA", 0},
        {InputDeviceType::RTLSDR, "00000001", 0},
        {InputDeviceType::AIRSPY, "0000000000000002", 1},
    };

    const auto ordered = orderDevices(input);
    check(ordered.size() == 4, "size preserved");
    if (ordered.size() == 4) {
        check(ordered[0].type == InputDeviceType::AIRSPY && ordered[0].serial == "0000000000000002",
              "airspy lowest serial first");
        check(ordered[1].type == InputDeviceType::AIRSPY && ordered[1].serial == "00000000000000AA",
              "airspy highest serial second");
        check(ordered[2].type == InputDeviceType::RTLSDR && ordered[2].serial == "00000001",
              "rtlsdr lowest serial after airspy");
        check(ordered[3].type == InputDeviceType::RTLSDR && ordered[3].serial == "00000002",
              "rtlsdr highest serial last");
    }

    // Ordering must be stable for equal serials so enumeration order is kept.
    std::vector<DeviceDescriptor> ties = {
        {InputDeviceType::RTLSDR, "same", 5},
        {InputDeviceType::RTLSDR, "same", 3},
    };
    const auto tieOrder = orderDevices(ties);
    check(tieOrder.size() == 2 && tieOrder[0].index == 5 && tieOrder[1].index == 3, "stable for equal serials");

    return failures == 0 ? 0 : 1;
}
