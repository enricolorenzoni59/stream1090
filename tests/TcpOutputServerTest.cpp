#include "ModeSFrameEncoder.hpp"
#include "TcpOutputServer.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <thread>
#include <atomic>
#include <sys/wait.h>
#include <signal.h>
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

#ifdef STREAM1090_READSB_TEST_EXECUTABLE
void realReadsbAcceptsFrame(bool beast) {
    TcpOutputConfig config;
    config.bindAddress = "127.0.0.1";
    config.enableAvr = !beast;
    config.enableBeast = beast;
    TcpOutputServer server(config);
    std::string error;
    assert(server.start(error));

    int output[2];
    assert(::pipe(output) == 0);
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        ::close(output[0]);
        ::dup2(output[1], STDOUT_FILENO);
        ::dup2(output[1], STDERR_FILENO);
        ::close(output[1]);
        const uint16_t port = beast ? server.beastPort() : server.avrPort();
        const std::string connector = "--net-connector=127.0.0.1," + std::to_string(port) +
                                      (beast ? ",beast_in" : ",raw_in");
        ::execl(STREAM1090_READSB_TEST_EXECUTABLE, STREAM1090_READSB_TEST_EXECUTABLE,
                "--net-only", connector.c_str(), "--no-interactive", "--show-only=40621d", nullptr);
        _exit(127);
    }
    ::close(output[1]);
    waitForClients(server, 1);

    const auto knownDf17 = ModeSFrame::longFrame(
        0x010203040506ULL, Bits128(0x00008D40621D58C3ULL, 0x82D690C8AC2863A7ULL), 0x60, true);
    for (int i = 0; i < 20; ++i) {
        assert(server.tryPublish(knownDf17));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ::kill(child, SIGTERM);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);

    std::string captured;
    std::array<char, 4096> chunk{};
    for (;;) {
        const ssize_t n = ::read(output[0], chunk.data(), chunk.size());
        if (n <= 0)
            break;
        captured.append(chunk.data(), size_t(n));
    }
    ::close(output[0]);
    server.stop();

    for (char& c : captured)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (captured.find("8d40621d58c382d690c8ac2863a7") == std::string::npos)
        std::fprintf(stderr, "readsb output did not contain the test frame:\n%s\n", captured.c_str());
    assert(captured.find("8d40621d58c382d690c8ac2863a7") != std::string::npos);
}
#endif
} // namespace

int main() {
    serverFanoutAndProtocols();
    orderedDeliveryAndReconnect();
    slowClientDoesNotBlockHealthyClient();
    promptStopAndInvalidBind();
#ifdef STREAM1090_READSB_TEST_EXECUTABLE
    realReadsbAcceptsFrame(false);
    realReadsbAcceptsFrame(true);
#endif
}
