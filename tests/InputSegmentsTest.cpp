/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "Global.hpp"
#include "Presets.hpp"
#include "InputBufferReader.hpp"
#include "SampleStream.hpp"
#include <future>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)

struct Handler {
    std::vector<uint64_t> times;
    void handleShort(uint64_t, uint64_t) { throw std::runtime_error("Unexpected short frame"); }
    void handleLong(uint64_t time, const Bits128&) { times.push_back(time); }
};

template<class Sampler> void checkSegments() {
    constexpr size_t Block = Sampler::InputBufferSize * 2;
    using Ring = RingBufferAsync<int16_t, Block, 4>;
    auto ring = std::make_unique<Ring>();
    typename Ring::Writer writer(*ring);
    writer.enableSegments(true);
    // A repeated known valid DF17, with deterministic quiet samples.
    const char* hex = "8D40621D58C382D690C8AC2863A7";
    const size_t samples = 16 * Sampler::InputBufferSize;
    std::vector<int16_t> iq(samples * 2);
    for (size_t n = 0; n < samples; ++n) {
        double t = double(n % 4000) / 4 - 100;
        bool pulse = (t >= 0 && t < .5) || (t >= 1 && t < 1.5)
            || (t >= 3.5 && t < 4) || (t >= 4.5 && t < 5);
        if (t >= 8 && t < 120) {
            const unsigned bit = unsigned(t - 8);
            const char c = hex[bit / 4];
            const unsigned nibble = c <= '9' ? c - '0' : c - 'A' + 10;
            pulse = ((nibble >> (3 - bit % 4)) & 1) ? (t - 8 - bit < .5) : (t - 8 - bit >= .5);
        }
        iq[2*n] = pulse ? 12000 : 0;
    }
    auto producer = std::async(std::launch::async, [&] {
        CHECK(writer.write(iq.data(), iq.size()) == iq.size());
        CHECK(writer.write(iq.data(), 6) == 6); // unpublished tail must not leak
        CHECK(writer.discontinuity(20));
        CHECK(writer.write(iq.data(), iq.size()) == iq.size());
        writer.shutdown();
    });
    auto pipeline = IQPipelineSelector<Rate_4_0_Mhz, Sampler::OutputSampleRate,
                                      IQPipelineOptions::BASEBAND_FIR>().make({});
    const auto initial = pipeline;
    InputBufferReader<IQ_INT16_FULL_SCALE, Block, 4, decltype(pipeline)> reader(pipeline, *ring);
    CHECK(reader.beginSegment()); CHECK(reader.firstComplexSample() == 0);
    Handler first;
    { SampleStream<Sampler> stream; stream.read(reader, first); }
    CHECK(first.times.size() > 20);
    CHECK(reader.beginSegment()); CHECK(reader.firstComplexSample() == samples + 13);
    pipeline = initial;
    Handler second;
    { SampleStream<Sampler> stream; stream.read(reader, second); }
    CHECK(first.times == second.times); // fresh FIR, resampler, decoder and cache
    CHECK(!reader.beginSegment()); producer.get();
    StdOutMessageHandler<Sampler> output(false);
    output.setInputSampleOffset(samples + 13);
    CHECK(output.m_timestampOffset == (samples + 13) * 3);
}
int main() {
    checkSegments<SamplerBase<Rate_4_0_Mhz, Rate_8_0_Mhz>>();
    checkSegments<SamplerBase<Rate_4_0_Mhz, Rate_12_0_Mhz>>();
}
