/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "InputReaderBase.hpp"
#include "RawInputFormat.hpp"
#include "SampleStream.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using Sampler = Sampler_10_0_to_48_0_Mhz;

struct NoiseCase {
  uint64_t offset;
  uint64_t positions;
};

// Each window starts 10 ms before the minimum pre-roll found by bisection and
// ends 10 ms after the known emission. Any frame emitted from pure noise fails
// the regression.
constexpr std::array<NoiseCase, 10> NoiseCases{{
    {385'046'780, 50'081'218},
    {385'046'635, 96'395'025},
    {962'286'190, 74'498'908},
    {1'109'091'320, 201'712},
    {1'188'570'535, 14'440'447},
    {1'217'535'165, 9'692'870},
    {1'278'588'175, 34'295'945},
    {1'318'201'410, 40'561'015},
    {1'443'420'445, 5'709'182},
    {1'713'099'945, 32'210'831},
}};

struct EmittedFrame {
  bool isLong;
  uint64_t high;
  uint64_t low;
};

struct CountingHandler {
  explicit CountingHandler(std::vector<EmittedFrame> &uniqueFrames)
      : m_uniqueFrames(uniqueFrames) {}

  void handleShort(uint64_t, uint64_t frame) {
    ++frameCount;
    logFrame({false, 0, frame});
  }

  void handleLong(uint64_t, const Bits128 &frame) {
    ++frameCount;
    logFrame({true, frame.high() & 0xffffffffffffull, frame.low()});
  }

  uint64_t frameCount{0};

private:
  void logFrame(const EmittedFrame &frame) {
    const auto seen = std::find_if(
        m_uniqueFrames.begin(), m_uniqueFrames.end(), [&](const auto &other) {
          return other.isLong == frame.isLong && other.high == frame.high &&
                 other.low == frame.low;
        });
    if (seen != m_uniqueFrames.end())
      return;

    m_uniqueFrames.push_back(frame);
    std::printf("forbidden frame: ");
    if (frame.isLong) {
      std::printf("%012llX%016llX", static_cast<unsigned long long>(frame.high),
                  static_cast<unsigned long long>(frame.low));
    } else {
      std::printf("%014llX", static_cast<unsigned long long>(frame.low));
    }
    std::printf("\n");
  }

  std::vector<EmittedFrame> &m_uniqueFrames;
};

// SplitMix64 makes every sample a pure function of its absolute index. This
// lets each test seek directly to a known failure without generating the long
// prefix before it. The low five bits give uniform white noise centred on the
// Airspy midpoint, at about -47 dBFS RMS.
uint64_t noiseWord(uint64_t index) noexcept {
  uint64_t x = index + 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

struct NoPipeline {
  static constexpr bool isEmpty = true;
};

class NoiseReader
    : public InputReaderBase<IQ_UINT16_RAW_AIRSPY, Sampler::InputBufferSize,
                             NoPipeline> {
public:
  NoiseReader(NoPipeline &pipeline, uint64_t offset, uint64_t positions)
      : InputReaderBase<IQ_UINT16_RAW_AIRSPY, Sampler::InputBufferSize,
                        NoPipeline>(pipeline),
        m_offset(offset), m_remaining(positions) {}

  void readMagnitude(int32_t *out) noexcept {
    const size_t count =
        std::min<uint64_t>(m_remaining, Sampler::InputBufferSize);
    for (size_t i = 0; i < count; ++i) {
      for (size_t lane = 0; lane < 2; ++lane) {
        const uint64_t rawIndex = 2 * (m_offset + i) + lane;
        const int delta = int(noiseWord(rawIndex) & 31u) - 16;
        m_raw[2 * i + lane] = uint16_t(2048 + delta);
      }
    }

    // Match InputStdStreamReader: a short final block is zero-padded in the
    // raw unsigned format before it is converted to magnitude.
    std::fill(m_raw.begin() + 2 * count, m_raw.end(), uint16_t(0));
    this->processBlock(m_raw.data(), out);
    m_offset += count;
    m_remaining -= count;
  }

  bool eof() const noexcept { return m_remaining == 0; }

private:
  uint64_t m_offset;
  uint64_t m_remaining;
  std::array<uint16_t, 2 * Sampler::InputBufferSize> m_raw{};
};

} // namespace

int main() {
  size_t failedCases = 0;
  std::vector<EmittedFrame> uniqueFrames;

  for (size_t i = 0; i < NoiseCases.size(); ++i) {
    const auto &test = NoiseCases[i];
    NoPipeline pipeline;
    NoiseReader reader(pipeline, test.offset, test.positions);
    CountingHandler handler(uniqueFrames);
    SampleStream<Sampler> stream;
    stream.read(reader, handler);

    if (handler.frameCount == 0)
      continue;

    ++failedCases;
    std::printf("FAILED case %zu: pure noise emitted %llu frame%s\n", i + 1,
                static_cast<unsigned long long>(handler.frameCount),
                handler.frameCount == 1 ? "" : "s");
  }

  if (failedCases == 0)
    return 0;

  std::printf(
      "FAILED: %zu of %zu pure-noise windows emitted %zu distinct frames\n",
      failedCases, NoiseCases.size(), uniqueFrames.size());
  return 1;
}
