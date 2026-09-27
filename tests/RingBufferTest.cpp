/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "RingBuffer.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <vector>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

void testWriterResumesAfterOneBlockIsConsumed() {
    using Ring = RingBufferAsync<int, 1, 2>;

    Ring ring;
    Ring::Writer writer(ring);
    Ring::Reader reader(ring);

    const int initial[] = {1, 2};
    writer.write(initial, 2);

    auto pendingWrite = std::async(std::launch::async, [&] {
        const int value = 3;
        writer.write(&value, 1);
    });

    require(!reader.eof());
    reader.process([](const int*) {});

    const auto status = pendingWrite.wait_for(std::chrono::seconds(1));
    if (status != std::future_status::ready) {
        reader.process([](const int*) {});
        pendingWrite.wait();
    }

    require(status == std::future_status::ready);
}

void testSegmentsDiscardOnlyUnpublishedTail() {
    using Ring = RingBufferAsync<int, 4, 2>;
    Ring ring; Ring::Writer writer(ring); Ring::Reader reader(ring);
    const int old[] = {1,2,3,4,5,6};
    require(writer.write(old, 6) == 6);
    require(!writer.discontinuity(10)); // opt-in, captures remain strict
    writer.enableSegments(true);
    require(writer.discontinuity(10));
    require(writer.discontinuity(2)); // no intervening full block
    const int next[] = {7,8,9,10};
    require(writer.write(next, 4) == 4);
    writer.shutdown();
    require(!reader.eof()); require(reader.segment().epoch == 0);
    reader.process([](const int* p) { require(p[0] == 1 && p[3] == 4); });
    require(!reader.eof()); require(reader.segment().epoch == 2);
    require(reader.segment().scalarOffset == 18);
    reader.process([](const int* p) { require(p[0] == 7 && p[3] == 10); });
    require(reader.eof()); require(!writer.discontinuity(1));
}

void testFullQueueBoundariesAndWrap() {
    using Ring = RingBufferAsync<uint64_t, 4, 2>;
    Ring ring; Ring::Writer writer(ring); Ring::Reader reader(ring);
    writer.enableSegments(true);
    auto producer = std::async(std::launch::async, [&] {
        for (uint64_t epoch = 0; epoch < 10000; ++epoch) {
            if (epoch) require(writer.discontinuity(2));
            const uint64_t block[] = {epoch,epoch,epoch,epoch};
            require(writer.write(block, 4) == 4);
        }
        writer.shutdown();
    });
    uint64_t epoch = 0;
    while (!reader.eof()) {
        require(reader.segment().epoch == epoch);
        require(reader.segment().scalarOffset == epoch * 6);
        reader.process([&](const uint64_t* p) {
            for (size_t n = 0; n < 4; ++n) require(p[n] == epoch);
        });
        ++epoch;
    }
    producer.get(); require(epoch == 10000);
}

} // namespace

int main() {
    testWriterResumesAfterOneBlockIsConsumed();
    testSegmentsDiscardOnlyUnpublishedTail();
    testFullQueueBoundariesAndWrap();
}
