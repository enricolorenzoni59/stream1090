/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <sdrplay_api.h>

// SDK-specific capabilities at 1090 MHz. Shared by discovery and opening.
struct SdrplayModel {
    const char* name;
    int maxLnaState;
    static SdrplayModel lookup(unsigned id) {
        switch (id) {
        case SDRPLAY_RSP1B_ID: return {"RSP1B", 8};
        case SDRPLAY_RSPduo_ID: return {"RSPduo", 8};
        case SDRPLAY_RSPdx_ID: return {"RSPdx", 18};
        default: return {nullptr, 0};
        }
    }
    static bool available(const sdrplay_api_DeviceT& device) {
        return lookup(device.hwVer).name && device.valid &&
            (device.hwVer != SDRPLAY_RSPduo_ID ||
             (device.rspDuoMode & sdrplay_api_RspDuoMode_Single_Tuner));
    }
};
