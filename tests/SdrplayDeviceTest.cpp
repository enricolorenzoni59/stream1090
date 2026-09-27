/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "devices/SdrplayDevice.hpp"
#include "Metrics.hpp"
#include "devices/SdrplayModel.hpp"
#include <vector>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(0)
struct Writer : IAsyncWriter<int16_t> {
    std::vector<int16_t> values;
    bool segments = false;
    bool discard = false;
    uint64_t missing = 0;
    bool discontinuity(uint64_t n) override { missing += n; return segments; }
    size_t write(const int16_t* p, size_t n) override { if (!discard) values.insert(values.end(),p,p+n); return n; }
    void shutdown() override { probe_writer_shutdown=true; }
};
int main() {
    {
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w);
        CHECK(d.open()); CHECK(d.start());
        short i[]={1,2,3,4},q[]={-1,-2,-3,-4};
        sdrplay_api_StreamCbParamsT p{0xfffffffcu};
        probe_callbacks.StreamACbFn(i,q,&p,4,1,probe_context);
        CHECK(w.values == std::vector<int16_t>({1,-1,2,-2,3,-3,4,-4}));
        p.firstSampleNum=0; probe_callbacks.StreamACbFn(i,q,&p,4,0,probe_context);
        CHECK(!d.streamFailed()); CHECK(w.values.size()==16); // counter rollover
        sdrplay_api_EventParamsT event{};
        probe_callbacks.EventCbFn(sdrplay_api_PowerOverloadChange,1,&event,probe_context);
        CHECK(probe_updates==1); CHECK(d.captureMetadata().at("overload_events")=="1");
        p.firstSampleNum=8; probe_callbacks.StreamACbFn(i,q,&p,4,0,probe_context);
        CHECK(d.streamFailed()); CHECK(w.values.size()==16); // do not concatenate gap
        CHECK(d.captureMetadata().at("missing_samples_modulo_2_32")=="4");
        d.close(); d.close(); CHECK(probe_uninit==1); CHECK(probe_release==1);
        CHECK(probe_shutdown_at_uninit);
    }
    {
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); CHECK(d.open()); CHECK(d.start());
        short i[]={1},q[]={2}; sdrplay_api_StreamCbParamsT p{};
        probe_callbacks.StreamACbFn(i,q,&p,1,0,probe_context);
        probe_callbacks.StreamACbFn(i,q,&p,1,1,probe_context);
        CHECK(d.streamFailed()); CHECK(w.values.size()==2);
    }
    {
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); CHECK(d.open());
        probe_init_error=1; CHECK(!d.start()); probe_init_error=0;
    }
    {
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); CHECK(d.open()); CHECK(d.start());
        std::vector<short> i(10000,123),q(10000,-456); sdrplay_api_StreamCbParamsT p{};
        probe_callbacks.StreamACbFn(i.data(),q.data(),&p,i.size(),0,probe_context);
        CHECK(w.values.size()==20000);
        for(size_t n=0;n<w.values.size();n+=2) { CHECK(w.values[n]==123); CHECK(w.values[n+1]==-456); }
        probe_callbacks.EventCbFn(sdrplay_api_DeviceRemoved,1,nullptr,probe_context);
        CHECK(d.streamFailed());
    }
    {
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); CHECK(d.open()); CHECK(d.start());
        d.close(); // The SDK double emits DeviceFailure during Uninit.
        CHECK(!d.streamFailed()); CHECK(d.captureMetadata().at("failure_reason")=="none");
    }
    {
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); DeviceConfig cfg; cfg.sdrplay.usbBulk=true;
        d.applyConfigPreOpen(cfg); CHECK(d.open());
        sdrplay_api_DeviceParamsT* p=nullptr; sdrplay_api_GetDeviceParams(nullptr,&p);
        CHECK(p->devParams->mode==sdrplay_api_BULK);
        CHECK(d.captureMetadata().at("usb_transfer_mode")=="bulk");
    }
    {
        auto& reg=Metrics::registry();
        const auto beforeMissing=reg.sdrplayMissingSamples.get();
        const auto beforeErrors=reg.sdrplaySequenceErrors.get();
        const auto beforeCount=reg.sdrplayGapDuration.count.load();
        const auto beforeDuration=reg.sdrplayGapDuration.sum.load();
        Writer w; w.segments=true; SdrplayDevice d(Rate_4_0_Mhz,w);
        CHECK(d.open()); CHECK(d.start());
        short i[]={1,2}, q[]={3,4}; sdrplay_api_StreamCbParamsT p{100};
        probe_callbacks.StreamACbFn(i,q,&p,2,0,probe_context);
        p.firstSampleNum=110;
        probe_callbacks.StreamACbFn(i,q,&p,2,0,probe_context);
        CHECK(!d.streamFailed()); CHECK(w.missing==16); CHECK(w.values.size()==8);
        CHECK(reg.sdrplayMissingSamples.get()==beforeMissing+8);
        CHECK(reg.sdrplayGapDuration.count.load()==beforeCount+1);
        CHECK(std::abs(reg.sdrplayGapDuration.sum.load()-beforeDuration-0.000002)<1e-12);
        CHECK(reg.sdrplayLargestGap.get()>=0.000002);
        CHECK(d.captureMetadata().at("gap_events")=="1");
        p.firstSampleNum=112;
        probe_callbacks.StreamACbFn(i,q,&p,2,0,probe_context);
        CHECK(w.values.size()==12); CHECK(w.missing==16);
        // A backward counter cannot preserve the timeline.
        p.firstSampleNum=100;
        probe_callbacks.StreamACbFn(i,q,&p,2,0,probe_context);
        CHECK(d.streamFailed()); CHECK(w.values.size()==12);
        CHECK(reg.sdrplayMissingSamples.get()==beforeMissing+8);
        CHECK(reg.sdrplaySequenceErrors.get()==beforeErrors+1);
        CHECK(reg.sdrplayGapDuration.count.load()==beforeCount+1);
    }
    {
        auto& reg = Metrics::registry();
        Writer w; w.segments=true; w.discard=true;
        SdrplayDevice d(Rate_4_0_Mhz,w); CHECK(d.open()); CHECK(d.start());
        CHECK(reg.sdrplayIqValid.get()==0 && reg.sdrplayOverloadActive.get()==-1);
        std::vector<short> i(4000000,1024),q(4000000,-1024);
        sdrplay_api_StreamCbParamsT p{};
        probe_callbacks.StreamACbFn(i.data(),q.data(),&p,4000000,0,probe_context);
        CHECK(reg.sdrplayIqWindows.get()==0); // exporter collection disabled
        reg.setSignalQualityCollection(true);
        p.firstSampleNum=4000000;
        probe_callbacks.StreamACbFn(i.data(),q.data(),&p,4000000,0,probe_context);
        CHECK(reg.sdrplayIqWindows.get()==1 && reg.sdrplayIqValid.get()==1);
        CHECK(reg.sdrplayIqScalars.get()==125000 && reg.sdrplayIqRails.get()==0);
        CHECK(std::fabs(reg.sdrplayIqRms.get()-20*std::log10(1.0/32))<1e-9);
        p.firstSampleNum=8000100;
        probe_callbacks.StreamACbFn(i.data(),q.data(),&p,100,0,probe_context);
        CHECK(reg.sdrplayIqValid.get()==0 && reg.sdrplayIqWindows.get()==1);
        sdrplay_api_EventParamsT event{};
        probe_callbacks.EventCbFn(sdrplay_api_PowerOverloadChange,1,&event,probe_context);
        CHECK(reg.sdrplayOverloadActive.get()==1);
        event.powerOverloadParams.powerOverloadChangeType=sdrplay_api_Overload_Corrected;
        probe_callbacks.EventCbFn(sdrplay_api_PowerOverloadChange,1,&event,probe_context);
        CHECK(reg.sdrplayOverloadActive.get()==0);
        CHECK(d.captureMetadata().at("overload_events")=="1");
        d.close(); CHECK(reg.sdrplayIqValid.get()==0 && reg.sdrplayOverloadActive.get()==-1);
        reg.setSignalQualityCollection(false);
    }
    CHECK(probe_release==8); CHECK(probe_uninit==6);
    // New models share the IQ path, but must configure their own API blocks.
    for (int tuner : {1, 2}) {
        probe_device = {}; probe_device.hwVer=SDRPLAY_RSPduo_ID; probe_device.tuner=3;
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); DeviceConfig cfg;
        cfg.sdrplay.tuner=tuner; cfg.sdrplay.rfNotch=true; cfg.sdrplay.dabNotch=true;
        cfg.sdrplay.lnaState=8; cfg.biasTee=tuner==2;
        d.applyConfigPreOpen(cfg); CHECK(d.open()); CHECK(d.start());
        CHECK(probe_selected.rspDuoMode==sdrplay_api_RspDuoMode_Single_Tuner);
        CHECK(probe_selected.tuner==tuner && probe_selected.rspDuoSampleFreq==0);
        const auto& active=tuner==1 ? probe_rx_a : probe_rx_b;
        const auto& inactive=tuner==1 ? probe_rx_b : probe_rx_a;
        CHECK(active.tunerParams.rfFreq.rfHz==1090000000);
        CHECK(active.tunerParams.gain.LNAstate==8);
        CHECK(inactive.tunerParams.rfFreq.rfHz==0);
        CHECK(active.rspDuoTunerParams.rfNotchEnable && active.rspDuoTunerParams.rfDabNotchEnable);
        CHECK(active.rspDuoTunerParams.biasTEnable==(tuner==2));
        CHECK(active.rspDuoTunerParams.tuner1AmPortSel==sdrplay_api_RspDuo_AMPORT_2);
        CHECK(!active.rsp1aTunerParams.biasTEnable && !probe_dev_params.rsp1aParams.rfNotchEnable);
        // The API single-tuner stream uses callback A, also for tuner B.
        short i[]={12}, q[]={-34}; sdrplay_api_StreamCbParamsT params{};
        probe_callbacks.StreamACbFn(i,q,&params,1,1,probe_context);
        CHECK(w.values==std::vector<int16_t>({12,-34})); CHECK(!d.streamFailed());
        CHECK(d.captureMetadata().at("device")=="RSPduo");
        CHECK(d.captureMetadata().at("tuner")==std::to_string(tuner));
        probe_callbacks.StreamBCbFn(i,q,&params,1,0,probe_context);
        CHECK(d.streamFailed()); CHECK(w.values.size()==2);
        CHECK(d.captureMetadata().at("failure_reason")=="unexpected_second_stream");
    }
    for (const std::string antenna : {"", "A", "B"}) {
        probe_device={}; probe_device.hwVer=SDRPLAY_RSPdx_ID;
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); DeviceConfig cfg;
        cfg.sdrplay.antenna=antenna; cfg.sdrplay.lnaState=18;
        cfg.sdrplay.rfNotch=true; cfg.sdrplay.dabNotch=true; cfg.biasTee=antenna=="B";
        d.applyConfigPreOpen(cfg); CHECK(d.open()); CHECK(d.start());
        CHECK(probe_dev_params.rspDxParams.antennaSel==(antenna=="B" ? sdrplay_api_RspDx_ANTENNA_B : sdrplay_api_RspDx_ANTENNA_A));
        CHECK(probe_dev_params.rspDxParams.biasTEnable==(antenna=="B"));
        CHECK(!probe_dev_params.rspDxParams.hdrEnable);
        CHECK(probe_dev_params.rspDxParams.rfNotchEnable && probe_dev_params.rspDxParams.rfDabNotchEnable);
        CHECK(!probe_dev_params.rsp1aParams.rfNotchEnable && !probe_rx_a.rsp1aTunerParams.biasTEnable);
        CHECK(probe_rx_a.tunerParams.gain.LNAstate==18);
        CHECK(d.captureMetadata().at("device")=="RSPdx");
        CHECK(d.captureMetadata().at("antenna")== (antenna=="B" ? "B" : "A"));
    }
    auto reject = [](int model, DeviceConfig cfg) {
        probe_device={}; probe_device.hwVer=model; probe_device.tuner=3;
        const int before=probe_selects;
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); d.applyConfigPreOpen(cfg);
        CHECK(!d.open()); CHECK(probe_selects==before);
    };
    DeviceConfig invalid;
    invalid.sdrplay.lnaState=9; reject(SDRPLAY_RSP1B_ID,invalid); reject(SDRPLAY_RSPduo_ID,invalid);
    invalid={}; invalid.sdrplay.tuner=2; reject(SDRPLAY_RSP1B_ID,invalid); reject(SDRPLAY_RSPdx_ID,invalid);
    invalid={}; invalid.sdrplay.antenna="B"; reject(SDRPLAY_RSP1B_ID,invalid); reject(SDRPLAY_RSPduo_ID,invalid);
    invalid={}; invalid.biasTee=true; reject(SDRPLAY_RSPduo_ID,invalid); reject(SDRPLAY_RSPdx_ID,invalid);
    reject(255,{}); // RSP1A is not implicitly enabled by this extension.
    for (int mode : {0, sdrplay_api_RspDuoMode_Slave}) {
        probe_device={}; probe_device.hwVer=SDRPLAY_RSPduo_ID; probe_device.rspDuoMode=mode;
        CHECK(!SdrplayModel::available(probe_device));
        const int before=probe_selects;
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); CHECK(!d.open()); CHECK(probe_selects==before);
    }
    {
        probe_device={}; probe_device.hwVer=SDRPLAY_RSPduo_ID; probe_device.tuner=1;
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); DeviceConfig cfg; cfg.sdrplay.tuner=2;
        d.applyConfigPreOpen(cfg); CHECK(!d.open()); // requested tuner unavailable
    }
    {
        probe_device={}; probe_device.hwVer=SDRPLAY_RSPduo_ID; probe_device.tuner=3; probe_null_b=true;
        const int before=probe_release;
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); DeviceConfig cfg; cfg.sdrplay.tuner=2;
        d.applyConfigPreOpen(cfg); CHECK(!d.open()); CHECK(probe_release==before+1);
        probe_null_b=false;
    }
    {
        probe_device={}; probe_device.valid=false;
        CHECK(!SdrplayModel::available(probe_device));
        Writer w; SdrplayDevice d(Rate_4_0_Mhz,w); CHECK(!d.open());
    }
    {
        probe_device={};
        auto& reg=Metrics::registry();
        const auto missing=reg.sdrplayMissingSamples.get();
        const auto errors=reg.sdrplaySequenceErrors.get();
        const auto count=reg.sdrplayGapDuration.count.load();
        Writer w; w.segments=true; SdrplayDevice d(Rate_4_0_Mhz,w);
        CHECK(d.open()); CHECK(d.start());
        short i[]={1,2,3,4},q[]={0,0,0,0};
        sdrplay_api_StreamCbParamsT p{0xfffffff8u};
        probe_callbacks.StreamACbFn(i,q,&p,4,0,probe_context); // next = fffffffc
        p.firstSampleNum=2; // six missing pairs across rollover
        probe_callbacks.StreamACbFn(i,q,&p,4,0,probe_context);
        CHECK(!d.streamFailed()); CHECK(w.missing==12);
        CHECK(reg.sdrplayMissingSamples.get()==missing+6);
        CHECK(reg.sdrplayGapDuration.count.load()==count+1);
        p.firstSampleNum=6u+0x80000000u; // exactly half-range: ambiguous
        probe_callbacks.StreamACbFn(i,q,&p,4,0,probe_context);
        CHECK(d.streamFailed()); CHECK(w.values.size()==16);
        CHECK(reg.sdrplayMissingSamples.get()==missing+6);
        CHECK(reg.sdrplaySequenceErrors.get()==errors+1);
        CHECK(reg.sdrplayGapDuration.count.load()==count+1);
    }

    probe_device={};

}
