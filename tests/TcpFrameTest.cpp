#include "ModeSFrame.hpp"
#include "ModeSFrameEncoder.hpp"
#include "SpscFrameQueue.hpp"

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
std::string asString(const EncodedModeSFrame& encoded) {
    return {reinterpret_cast<const char*>(encoded.bytes.data()), encoded.size};
}

std::vector<uint8_t> asBytes(const EncodedModeSFrame& encoded) {
    return {encoded.bytes.begin(), encoded.bytes.begin() + encoded.size};
}

void queueBasics() {
    static_assert(std::is_trivially_copyable_v<ModeSFrame>);
    SpscFrameQueue<4> queue;
    ModeSFrame out{};
    assert(!queue.tryPop(out));

    const auto a = ModeSFrame::shortFrame(1, 0x11223344556677ULL, 0x31, true);
    const auto b = ModeSFrame::longFrame(2, Bits128(0x0000AABBCCDDEEFFULL, 0x1122334455667788ULL), 0x32, true);
    const auto c = ModeSFrame::shortFrame(3, 0x01020304050607ULL, 0x33, true);
    const auto d = ModeSFrame::shortFrame(4, 0x11121314151617ULL, 0x34, true);

    assert(queue.tryPush(a));
    assert(queue.tryPush(b));
    assert(queue.tryPush(c));
    assert(queue.tryPush(d));
    assert(!queue.tryPush(a));
    assert(queue.tryPop(out) && out == a);
    assert(queue.tryPop(out) && out == b);
    assert(queue.tryPush(a)); // wrap around
    assert(queue.tryPop(out) && out == c);
    assert(queue.tryPop(out) && out == d);
    assert(queue.tryPop(out) && out == a);
    assert(!queue.tryPop(out));
}

void queueConcurrency() {
    constexpr uint64_t Count = 200000;
    SpscFrameQueue<1024> queue;
    std::thread producer([&] {
        for (uint64_t i = 0; i < Count; ++i) {
            const auto frame = ModeSFrame::shortFrame(i, i, uint8_t(i), true);
            while (!queue.tryPush(frame))
                std::this_thread::yield();
        }
    });

    for (uint64_t i = 0; i < Count; ++i) {
        ModeSFrame frame{};
        while (!queue.tryPop(frame))
            std::this_thread::yield();
        assert(frame.mlatTimestamp == i);
        assert(frame.low == i);
        assert(frame.signalLevel == uint8_t(i));
    }
    producer.join();
}

void avrEncoding() {
    auto shortNoRssi = ModeSFrame::shortFrame(0x123456789ABCULL, 0x8D40621D58C382ULL, 0x55, false);
    assert(asString(encodeAvr(shortNoRssi)) == "@123456789ABC8D40621D58C382;\n");

    auto shortRssi = ModeSFrame::shortFrame(0x123456789ABCULL, 0x8D40621D58C382ULL, 0x5A, true);
    assert(asString(encodeAvr(shortRssi)) == "<123456789ABC5A8D40621D58C382;\n");

    auto longNoRssi = ModeSFrame::longFrame(0x123456789ABCULL,
        Bits128(0x00008D40621D58C3ULL, 0x82D690C8AC2863A7ULL), 0, false);
    assert(asString(encodeAvr(longNoRssi)) == "@123456789ABC8D40621D58C382D690C8AC2863A7;\n");

    auto longRssi = longNoRssi;
    longRssi.signalLevel = 0x0F;
    longRssi.signalAvailable = true;
    assert(asString(encodeAvr(longRssi)) == "<123456789ABC0F8D40621D58C382D690C8AC2863A7;\n");

    shortNoRssi.mlatTimestamp = 0xFFFF123456789ABCULL;
    assert(asString(encodeAvr(shortNoRssi)).starts_with("@123456789ABC"));
}

void beastEncoding() {
    const auto shortFrame = ModeSFrame::shortFrame(0x010203040506ULL, 0x11223344556677ULL, 0x55, true);
    assert(asBytes(encodeBeast(shortFrame)) == std::vector<uint8_t>({
        0x1A, '2', 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x55,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}));

    const auto longFrame = ModeSFrame::longFrame(0x010203040506ULL,
        Bits128(0x0000112233445566ULL, 0x778899AABBCCDDEEULL), 0x44, true);
    assert(asBytes(encodeBeast(longFrame)) == std::vector<uint8_t>({
        0x1A, '3', 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x44,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE}));

    const auto escaped = ModeSFrame::shortFrame(0x001A02030405ULL, 0x111A334455661AULL, 0x1A, true);
    assert(asBytes(encodeBeast(escaped)) == std::vector<uint8_t>({
        0x1A, '2', 0x00, 0x1A, 0x1A, 0x02, 0x03, 0x04, 0x05,
        0x1A, 0x1A, 0x11, 0x1A, 0x1A, 0x33, 0x44, 0x55, 0x66, 0x1A, 0x1A}));
}
} // namespace

int main() {
    queueBasics();
    queueConcurrency();
    avrEncoding();
    beastEncoding();
}
