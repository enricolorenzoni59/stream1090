// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal behavioural double authored for tests. Not vendor headers; no ABI claim.
#pragma once
#define SDRPLAY_API_VERSION 3.15f
#define SDRPLAY_MAX_DEVICES 8
#define SDRPLAY_RSP1B_ID 6
#define SDRPLAY_RSPduo_ID 3
#define SDRPLAY_RSPdx_ID 4
constexpr int sdrplay_api_Tuner_A=1, sdrplay_api_Tuner_B=2;
constexpr int sdrplay_api_RspDuoMode_Single_Tuner=1, sdrplay_api_RspDuoMode_Slave=8;
constexpr int sdrplay_api_RspDuo_AMPORT_2=0;
constexpr int sdrplay_api_RspDx_ANTENNA_A=0, sdrplay_api_RspDx_ANTENNA_B=1;
using sdrplay_api_ErrT = int;
using sdrplay_api_TunerSelectT = int;
using sdrplay_api_ReasonForUpdateT = int;
using sdrplay_api_EventT = int;
constexpr int sdrplay_api_Success=0, sdrplay_api_Update_None=0;
constexpr int sdrplay_api_Update_Tuner_Frf=1, sdrplay_api_Update_Tuner_Gr=2;
constexpr int sdrplay_api_Update_Rsp1a_BiasTControl=3, sdrplay_api_Update_Ext1_None=0;
using sdrplay_api_Bw_MHzT = int;
using sdrplay_api_AdsbModeT = int;
constexpr int sdrplay_api_Update_Ctrl_OverloadMsgAck=9, sdrplay_api_Overload_Detected=0;
constexpr int sdrplay_api_Overload_Corrected=1;
constexpr int sdrplay_api_BW_5_000=5000, sdrplay_api_IF_Zero=0, sdrplay_api_AGC_DISABLE=0;
constexpr int sdrplay_api_ISOCH=0, sdrplay_api_BULK=1;
constexpr int sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_2MHZ=2;
constexpr int sdrplay_api_DeviceRemoved=3, sdrplay_api_DeviceFailure=4, sdrplay_api_PowerOverloadChange=1;
struct sdrplay_api_DeviceT { int hwVer=6; bool valid=true; void* dev=nullptr; int tuner=1; int rspDuoMode=1; double rspDuoSampleFreq=0; char SerNo[16]="REVIEW-STUB"; };
struct sdrplay_api_StreamCbParamsT { unsigned firstSampleNum=0; };
struct sdrplay_api_EventParamsT { struct { int powerOverloadChangeType=0; } powerOverloadParams; };
struct DevParams { struct { double fsHz; } fsFreq; double ppm; int mode; struct { bool rfNotchEnable,rfDabNotchEnable; } rsp1aParams; struct { bool rfNotchEnable,rfDabNotchEnable,biasTEnable,hdrEnable; int antennaSel; } rspDxParams; };
struct RxParams {
    struct { struct { double rfHz; } rfFreq; int bwType, ifType; struct { int gRdB, LNAstate; } gain; } tunerParams;
    struct { struct { int enable; } agc; int adsbMode; struct { int enable; } decimation; struct { int DCenable,IQenable; } dcOffset; } ctrlParams;
    struct { bool biasTEnable; } rsp1aTunerParams;
    struct { bool biasTEnable,rfNotchEnable,rfDabNotchEnable,tuner1AmNotchEnable; int tuner1AmPortSel; } rspDuoTunerParams;
};
struct sdrplay_api_DeviceParamsT { DevParams* devParams; RxParams* rxChannelA; RxParams* rxChannelB; };
struct sdrplay_api_CallbackFnsT {
    void (*StreamACbFn)(short*,short*,sdrplay_api_StreamCbParamsT*,unsigned,unsigned,void*)=nullptr;
    void (*StreamBCbFn)(short*,short*,sdrplay_api_StreamCbParamsT*,unsigned,unsigned,void*)=nullptr;
    void (*EventCbFn)(sdrplay_api_EventT,sdrplay_api_TunerSelectT,sdrplay_api_EventParamsT*,void*)=nullptr;
};
inline sdrplay_api_CallbackFnsT probe_callbacks;
inline void* probe_context;
inline bool probe_writer_shutdown=false, probe_shutdown_at_uninit=false;
inline sdrplay_api_DeviceT probe_device{}, probe_selected{};
inline int probe_selects=0;
inline bool probe_null_b=false;
inline DevParams probe_dev_params{};
inline RxParams probe_rx_a{}, probe_rx_b{};
inline int probe_updates=0, probe_release=0, probe_uninit=0, probe_init_error=0;
inline const char* sdrplay_api_GetErrorString(int) { return "stub"; }
inline int sdrplay_api_Open() { return 0; }
inline int sdrplay_api_ApiVersion(float* v) { *v=3.15f; return 0; }
inline int sdrplay_api_LockDeviceApi() { return 0; }
inline int sdrplay_api_UnlockDeviceApi() { return 0; }
inline int sdrplay_api_GetDevices(sdrplay_api_DeviceT* d,unsigned* n,unsigned) { *n=1; *d=probe_device; return 0; }
inline int sdrplay_api_SelectDevice(sdrplay_api_DeviceT* d) { ++probe_selects; probe_selected=*d; probe_dev_params={}; probe_rx_a={}; probe_rx_b={}; return 0; }
inline int sdrplay_api_GetDeviceParams(void*,sdrplay_api_DeviceParamsT** p) {
    static sdrplay_api_DeviceParamsT params; params={&probe_dev_params,&probe_rx_a,probe_null_b ? nullptr : &probe_rx_b}; *p=&params; return 0;
}
inline int sdrplay_api_Update(void*,int,int,int) { ++probe_updates; return 0; }
inline int sdrplay_api_Init(void*,sdrplay_api_CallbackFnsT* c,void* ctx) { probe_callbacks=*c; probe_context=ctx; return probe_init_error; }
inline int sdrplay_api_Uninit(void*) {
    ++probe_uninit; probe_shutdown_at_uninit=probe_writer_shutdown;
    probe_callbacks.EventCbFn(sdrplay_api_DeviceFailure,1,nullptr,probe_context);
    return 0;
}
inline int sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT*) { ++probe_release; return 0; }
inline int sdrplay_api_Close() { return 0; }
