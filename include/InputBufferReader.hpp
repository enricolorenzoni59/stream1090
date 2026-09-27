/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#pragma once

#include "InputReaderBase.hpp"
#include "RingBuffer.hpp"
#include "RawCapture.hpp"

template <typename RawFormat, size_t BufferBlockSize, size_t NumBufferBlocks, typename Pipeline>
class InputBufferReader : public InputReaderBase<RawFormat, BufferBlockSize / 2, Pipeline> {
  public:
    using RawType = typename RawFormat::RawType;
    using RingBufferType = RingBufferAsync<RawType, BufferBlockSize, NumBufferBlocks>;
    using AsyncReader = typename RingBufferType::Reader;

    InputBufferReader(Pipeline& pipeline, RingBufferType& ringBuffer, RawCapture* capture = nullptr)
        : InputReaderBase<RawFormat, BufferBlockSize / 2, Pipeline>(pipeline), m_reader(ringBuffer), capture_(capture) {}

    inline void readMagnitude(int32_t* out) {
        m_reader.process([&](const RawType* buffer) { record(buffer); this->processBlock(buffer, out); });
    }

    void readRaw() { m_reader.process([&](const RawType* buffer) { record(buffer); }); }

    bool beginSegment() {
        if (finished()) return false;
        epoch_ = m_reader.segment().epoch;
        firstScalar_ = m_reader.segment().scalarOffset;
        return true;
    }

    uint64_t firstComplexSample() const { return firstScalar_ / 2; }

    bool eof() {
        return finished() || m_reader.segment().epoch != epoch_;
    }

  private:
    AsyncReader m_reader;
    RawCapture* capture_;
    uint64_t epoch_ = 0, firstScalar_ = 0;
    bool finished() {
        return (capture_ && capture_->done()) || ProcessSignals::shutdownRequested() || m_reader.eof();
    }
    void record(const RawType* buffer) {
        if constexpr (std::is_same_v<RawType, int16_t>) {
            if (capture_) capture_->append(buffer, BufferBlockSize);
        }
    }
};
