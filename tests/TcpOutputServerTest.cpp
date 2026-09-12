#include "ModeSFrameEncoder.hpp"
#include "TcpOutputServer.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <vector>

namespace {
int connectLoopback(uint16_t port, int receiveBuffer = 0) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (receiveBuffer > 0) {
        // Set before connect so it caps the advertised window. Without it the
        // loopback buffers can absorb the whole test frame burst and the
        // server never sees a slow client, which is what made this test flaky.
        int value = receiveBuffer;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

std::vector<uint8_t> receiveExact(int fd, size_t count) {
    std::vector<uint8_t> bytes(count);
    size_t used = 0;
    while (used != count) {
        pollfd event{fd, POLLIN, 0};
        assert(::poll(&event, 1, 2000) == 1);
        const ssize_t n = ::recv(fd, bytes.data() + used, count - used, 0);
        assert(n > 0);
        used += size_t(n);
    }
    return bytes;
}

void waitForClients(TcpOutputServer& server, size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.clientCount() != expected && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(server.clientCount() == expected);
}

void serverFanoutAndProtocols() {
    TcpOutputConfig config;
    config.bindAddress = "127.0.0.1";
    config.avrPort = 0;   // ephemeral ports are available to the test API
    config.beastPort = 0;
    config.enableAvr = true;
    config.enableBeast = true;

    TcpOutputServer server(config);
    std::string error;
    if (!server.start(error)) {
        std::fprintf(stderr, "server start failed: %s\n", error.c_str());
        assert(false);
    }
    assert(server.avrPort() != 0);
    assert(server.beastPort() != 0);
    assert(server.avrPort() != server.beastPort());

    const int avr1 = connectLoopback(server.avrPort());
    const int avr2 = connectLoopback(server.avrPort());
    const int beast = connectLoopback(server.beastPort());
    waitForClients(server, 3);

    const auto frame = ModeSFrame::shortFrame(0x010203040506ULL, 0x11223344556677ULL, 0x55, true);
    const auto avrExpected = encodeAvr(frame);
    const auto beastExpected = encodeBeast(frame);
    assert(server.tryPublish(frame));

    assert(receiveExact(avr1, avrExpected.size) ==
           std::vector<uint8_t>(avrExpected.bytes.begin(), avrExpected.bytes.begin() + avrExpected.size));
    assert(receiveExact(avr2, avrExpected.size) ==
           std::vector<uint8_t>(avrExpected.bytes.begin(), avrExpected.bytes.begin() + avrExpected.size));
    assert(receiveExact(beast, beastExpected.size) ==
           std::vector<uint8_t>(beastExpected.bytes.begin(), beastExpected.bytes.begin() + beastExpected.size));

    ::close(avr1);
    ::close(avr2);
    ::close(beast);
    server.stop();
    assert(!server.running());
}

void orderedDeliveryAndReconnect() {
    TcpOutputConfig config;
    config.bindAddress = "127.0.0.1";
    config.enableAvr = true;
    config.avrPort = 0;
    TcpOutputServer server(config);
    std::string error;
    assert(server.start(error));

    int client = connectLoopback(server.avrPort());
    waitForClients(server, 1);
    const auto first = ModeSFrame::shortFrame(1, 0x01020304050607ULL, 1, false);
    const auto second = ModeSFrame::shortFrame(2, 0x11121314151617ULL, 2, false);
    const auto firstBytes = encodeAvr(first);
    const auto secondBytes = encodeAvr(second);
    assert(server.tryPublish(first));
    assert(server.tryPublish(second));
    auto received = receiveExact(client, firstBytes.size + secondBytes.size);
    std::vector<uint8_t> expected(firstBytes.bytes.begin(), firstBytes.bytes.begin() + firstBytes.size);
    expected.insert(expected.end(), secondBytes.bytes.begin(), secondBytes.bytes.begin() + secondBytes.size);
    assert(received == expected);

    ::shutdown(client, SHUT_RDWR);
    ::close(client);
    waitForClients(server, 0);
    client = connectLoopback(server.avrPort());
    waitForClients(server, 1);
    assert(server.tryPublish(second));
    assert(receiveExact(client, secondBytes.size) ==
           std::vector<uint8_t>(secondBytes.bytes.begin(), secondBytes.bytes.begin() + secondBytes.size));
    ::close(client);
    server.stop();
}

void slowClientDoesNotBlockHealthyClient() {
    TcpOutputConfig config;
    config.bindAddress = "127.0.0.1";
    config.enableAvr = true;
    config.avrPort = 0;
    // One network-thread drain appends the whole ring batch to every client
    // before any flush, and the ring holds at most 4096 frames (about 118 KB),
    // so the limit has to sit well above that or a busy drain disconnects the
    // healthy client too. The slow client, once its socket stops accepting,
    // keeps accumulating across drains and crosses the same limit.
    config.clientBufferLimit = 1024 * 1024;
    config.acceptedSocketSendBuffer = 1024;
    TcpOutputServer server(config);
    std::string error;
    assert(server.start(error));

    // The slow client gets a tiny receive window so the server's send() blocks
    // after a few KB instead of the kernel quietly buffering the whole burst.
    const int slow = connectLoopback(server.avrPort(), 4 * 1024);
    const int healthy = connectLoopback(server.avrPort());
    waitForClients(server, 2);

    // The burst has to be comfortably larger than the socket buffers the
    // kernel gives the non-reading client. macOS hands out ~319 KB and then
    // stops growing, while the slow client has to accumulate a full
    // clientBufferLimit (1 MB) of pending bytes before it is dropped, so the
    // total has to clear both. 80k frames is about 2.3 MB.
    constexpr size_t Count = 80000;
    const auto frame = ModeSFrame::shortFrame(0x010203040506ULL, 0x11223344556677ULL, 0x55, true);
    const auto encoded = encodeAvr(frame);
    std::atomic<size_t> received{0};
    std::thread reader([&] {
        std::array<uint8_t, 8192> bytes{};
        const size_t expected = Count * encoded.size;
        while (received.load(std::memory_order_relaxed) < expected) {
            const ssize_t n = ::recv(healthy, bytes.data(), bytes.size(), 0);
            assert(n > 0);
            received.fetch_add(size_t(n), std::memory_order_relaxed);
        }
    });

    for (size_t i = 0; i < Count; ++i) {
        while (!server.tryPublish(frame))
            std::this_thread::yield();
        // Pace the producer so one network-thread drain stays well under the
        // client buffer limit: the whole ring batch is appended to every
        // client before any of them is flushed, so a large batch would
        // disconnect the healthy client too.
        if ((i & 63) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    reader.join();
    assert(received.load() == Count * encoded.size);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.slowClientDisconnects() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(server.slowClientDisconnects() == 1);
    assert(server.clientCount() == 1);

    ::close(slow);
    ::close(healthy);
    server.stop();
}

void promptStopAndInvalidBind() {
    TcpOutputConfig config;
    config.bindAddress = "127.0.0.1";
    config.enableAvr = true;
    config.avrPort = 0;
    TcpOutputServer server(config);
    std::string error;
    assert(server.start(error));
    const uint16_t port = server.avrPort();

    auto conflictingConfig = config;
    conflictingConfig.avrPort = port;
    TcpOutputServer conflicting(conflictingConfig);
    assert(!conflicting.start(error));
    assert(!error.empty());

    const auto before = std::chrono::steady_clock::now();
    server.stop();
    assert(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250));

    TcpOutputServer restarted(conflictingConfig);
    assert(restarted.start(error));
    assert(restarted.avrPort() == port);
    restarted.stop();

    config.bindAddress = "not-an-address.invalid";
    TcpOutputServer invalid(config);
    assert(!invalid.start(error));
    assert(!error.empty());
}

// ---------------------------------------------------------------------------
// Independent wire-format decoders.
//
// The interop check used to shell out to readsb when it happened to be on
// PATH, so it silently disappeared on machines without it. These parsers are
// written from the protocol description, not from the encoder, so they still
// catch a wrong encoder while running everywhere.
// ---------------------------------------------------------------------------

struct ParsedFrame {
    uint64_t timestamp { 0 };
    bool hasSignal { false };
    uint8_t signal { 0 };
    bool isLong { false };
    uint64_t payload { 0 }; // short frame
    uint64_t high { 0 };    // long frame, top 48 bits
    uint64_t low { 0 };     // long frame, bottom 64 bits
};

/// One AVR line (without the trailing newline): '@' or '<', twelve timestamp
/// hex digits, an optional signal byte, then 14 (short) or 28 (long) payload
/// hex digits and ';'.
bool parseAvr(const std::string& line, ParsedFrame& out) {
    if (line.size() < 2 || line.back() != ';')
        return false;
    out.hasSignal = line[0] == '<';
    if (line[0] != '@' && line[0] != '<')
        return false;

    size_t pos = 1;
    const auto hex = [&](size_t digits, uint64_t& value) {
        if (pos + digits > line.size() - 1)
            return false;
        value = 0;
        for (size_t i = 0; i < digits; ++i) {
            const char c = line[pos++];
            const int d = (c >= '0' && c <= '9')   ? c - '0'
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                   : -1;
            if (d < 0)
                return false;
            value = (value << 4) | uint64_t(d);
        }
        return true;
    };

    if (!hex(12, out.timestamp))
        return false;
    uint64_t signal = 0;
    if (out.hasSignal && !hex(2, signal))
        return false;
    out.signal = uint8_t(signal);

    const size_t payloadDigits = (line.size() - 1) - pos;
    if (payloadDigits == 14) {
        out.isLong = false;
        return hex(14, out.payload) && pos == line.size() - 1;
    }
    if (payloadDigits == 28) {
        out.isLong = true;
        return hex(12, out.high) && hex(16, out.low);
    }
    return false;
}

/// One Beast message starting at `start`. Returns 1 on success, 0 when more
/// bytes are needed, -1 on a malformed stream. A body 0x1a must be doubled; a
/// lone 0x1a there is corruption, not a new start marker.
int parseBeast(const std::vector<uint8_t>& bytes, size_t start, ParsedFrame& out, size_t& consumed) {
    if (start >= bytes.size())
        return 0;
    if (bytes[start] != 0x1A)
        return -1;
    if (start + 1 >= bytes.size())
        return 0;

    const uint8_t type = bytes[start + 1];
    const size_t payloadBytes = (type == '2') ? 7 : (type == '3') ? 14 : 0;
    if (payloadBytes == 0)
        return -1;

    const size_t logicalBytes = 6 + 1 + payloadBytes;
    std::array<uint8_t, 21> body{};
    size_t got = 0;
    size_t pos = start + 2;
    while (got < logicalBytes) {
        if (pos >= bytes.size())
            return 0;
        const uint8_t b = bytes[pos++];
        if (b == 0x1A) {
            if (pos >= bytes.size())
                return 0;
            if (bytes[pos] != 0x1A)
                return -1;
            ++pos;
        }
        body[got++] = b;
    }

    out.timestamp = 0;
    for (size_t i = 0; i < 6; ++i)
        out.timestamp = (out.timestamp << 8) | body[i];
    out.hasSignal = true;
    out.signal = body[6];
    out.isLong = (type == '3');
    if (out.isLong) {
        for (size_t i = 0; i < 6; ++i)
            out.high = (out.high << 8) | body[7 + i];
        for (size_t i = 0; i < 8; ++i)
            out.low = (out.low << 8) | body[13 + i];
    } else {
        for (size_t i = 0; i < 7; ++i)
            out.payload = (out.payload << 8) | body[7 + i];
    }
    consumed = pos - start;
    return 1;
}

std::vector<std::string> readAvrFrames(int fd, size_t count) {
    std::string buffer;
    std::vector<std::string> lines;
    while (lines.size() < count) {
        pollfd event{ fd, POLLIN, 0 };
        assert(::poll(&event, 1, 3000) == 1);
        char chunk[4096];
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        assert(n > 0);
        buffer.append(chunk, size_t(n));
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            lines.push_back(buffer.substr(0, newline));
            buffer.erase(0, newline + 1);
        }
    }
    return lines;
}

std::vector<ParsedFrame> readBeastFrames(int fd, size_t count) {
    std::vector<uint8_t> buffer;
    std::vector<ParsedFrame> frames;
    while (frames.size() < count) {
        pollfd event{ fd, POLLIN, 0 };
        assert(::poll(&event, 1, 3000) == 1);
        uint8_t chunk[4096];
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        assert(n > 0);
        buffer.insert(buffer.end(), chunk, chunk + n);

        size_t pos = 0;
        while (pos < buffer.size()) {
            ParsedFrame frame;
            size_t consumed = 0;
            const int result = parseBeast(buffer, pos, frame, consumed);
            if (result == 0)
                break;
            assert(result == 1); // a lone 0x1a in the body would break the framing
            frames.push_back(frame);
            pos += consumed;
        }
        buffer.erase(buffer.begin(), buffer.begin() + ptrdiff_t(pos));
    }
    return frames;
}

/// Both wire formats, decoded by the independent parsers above, byte for byte.
/// The frames carry 0x1a in the timestamp, the signal and the payload so the
/// Beast escaping is exercised rather than assumed.
void referenceDecoderValidatesWireFormat() {
    TcpOutputConfig config;
    config.bindAddress = "127.0.0.1";
    config.avrPort = 0;
    config.beastPort = 0;
    config.enableAvr = true;
    config.enableBeast = true;

    TcpOutputServer server(config);
    std::string error;
    assert(server.start(error));

    const int avr = connectLoopback(server.avrPort());
    const int beast = connectLoopback(server.beastPort());
    waitForClients(server, 2);

    const std::vector<ModeSFrame> frames {
        ModeSFrame::shortFrame(0x001A02030405ULL, 0x111A334455661AULL, 0x1A, true),
        ModeSFrame::shortFrame(0x010203040506ULL, 0x11223344556677ULL, 0x00, false),
        ModeSFrame::longFrame(0x010203040506ULL, Bits128(0x0000112233445566ULL, 0x778899AABBCCDDEEULL), 0x44, true),
    };
    for (const auto& frame : frames)
        assert(server.tryPublish(frame));

    const auto avrLines = readAvrFrames(avr, frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        ParsedFrame parsed;
        assert(parseAvr(avrLines[i], parsed));
        const bool isLong = frames[i].length == ModeSFrameLength::Long;
        assert(parsed.timestamp == frames[i].mlatTimestamp);
        assert(parsed.isLong == isLong);
        assert(parsed.hasSignal == frames[i].signalAvailable);
        if (frames[i].signalAvailable)
            assert(parsed.signal == frames[i].signalLevel);
        if (isLong) {
            assert(parsed.high == frames[i].high);
            assert(parsed.low == frames[i].low);
        } else {
            assert(parsed.payload == frames[i].low);
        }
    }

    const auto beastFrames = readBeastFrames(beast, frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        const bool isLong = frames[i].length == ModeSFrameLength::Long;
        assert(beastFrames[i].timestamp == frames[i].mlatTimestamp);
        assert(beastFrames[i].isLong == isLong);
        assert(beastFrames[i].signal == (frames[i].signalAvailable ? frames[i].signalLevel : 0));
        if (isLong) {
            assert(beastFrames[i].high == frames[i].high);
            assert(beastFrames[i].low == frames[i].low);
        } else {
            assert(beastFrames[i].payload == frames[i].low);
        }
    }

    ::close(avr);
    ::close(beast);
    server.stop();
}
} // namespace

int main() {
    serverFanoutAndProtocols();
    orderedDeliveryAndReconnect();
    slowClientDoesNotBlockHealthyClient();
    promptStopAndInvalidBind();
    referenceDecoderValidatesWireFormat();
}
