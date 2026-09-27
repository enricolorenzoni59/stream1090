/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "devices/SdrplayDevice.hpp"
#include "Metrics.hpp"
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
        Writer w; w.segments=true; SdrplayDevice d(Rate_4_0_Mhz,w);
        CHECK(d.open()); CHECK(d.start());
        short i[]={1,2}, q[]={3,4}; sdrplay_api_StreamCbParamsT p{100};
        probe_callbacks.StreamACbFn(i,q,&p,2,0,probe_context);
        p.firstSampleNum=110;
        probe_callbacks.StreamACbFn(i,q,&p,2,0,probe_context);
        CHECK(!d.streamFailed()); CHECK(w.missing==16); CHECK(w.values.size()==8);
        CHECK(d.captureMetadata().at("gap_events")=="1");
        p.firstSampleNum=112;
        probe_callbacks.StreamACbFn(i,q,&p,2,0,probe_context);
        CHECK(w.values.size()==12); CHECK(w.missing==16);
        // A backward counter cannot preserve the timeline.
        p.firstSampleNum=100;
        probe_callbacks.StreamACbFn(i,q,&p,2,0,probe_context);
        CHECK(d.streamFailed()); CHECK(w.values.size()==12);
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
}
