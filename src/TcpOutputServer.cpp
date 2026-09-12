/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "TcpOutputServer.hpp"

#include "ModeSFrameEncoder.hpp"
#include "SpscFrameQueue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
enum class Protocol { Avr, Beast };

struct Client {
    int fd = -1;
    Protocol protocol = Protocol::Avr;
    std::vector<uint8_t> pending;
    size_t offset = 0;
};

bool setNonBlockingCloseOnExec(int fd) {
    const int status = ::fcntl(fd, F_GETFL, 0);
    const int descriptor = ::fcntl(fd, F_GETFD, 0);
    return status >= 0 && descriptor >= 0 && ::fcntl(fd, F_SETFL, status | O_NONBLOCK) == 0 &&
           ::fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) == 0;
}

void closeFd(int& fd) noexcept {
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

int sendFlags() noexcept {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
} // namespace

struct TcpOutputServer::Impl {
    explicit Impl(TcpOutputConfig value) : config(std::move(value)) {}

    TcpOutputConfig config;
    SpscFrameQueue<4096> frames;
    std::atomic<bool> isRunning{false};
    std::atomic<size_t> connectedClients{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> slowDisconnects{0};
    int controlRead = -1;
    int controlWrite = -1;
    int avrListener = -1;
    int beastListener = -1;
    uint16_t actualAvrPort = 0;
    uint16_t actualBeastPort = 0;
    std::thread worker;
    std::vector<Client> clients;

    int createListener(const std::string& address, uint16_t requestedPort, uint16_t& actualPort,
                       std::string& error) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        const std::string port = std::to_string(requestedPort);
        addrinfo* results = nullptr;
        const int lookup = ::getaddrinfo(address.empty() ? nullptr : address.c_str(), port.c_str(), &hints, &results);
        if (lookup != 0) {
            error = "cannot resolve bind address '" + address + "': " + ::gai_strerror(lookup);
            return -1;
        }

        int listener = -1;
        int lastError = 0;
        for (addrinfo* candidate = results; candidate; candidate = candidate->ai_next) {
            listener = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
            if (listener < 0) {
                lastError = errno;
                continue;
            }
            const int yes = 1;
            ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            if (!setNonBlockingCloseOnExec(listener) ||
                ::bind(listener, candidate->ai_addr, candidate->ai_addrlen) != 0 || ::listen(listener, 8) != 0) {
                lastError = errno;
                closeFd(listener);
                continue;
            }
            break;
        }
        ::freeaddrinfo(results);

        if (listener < 0) {
            error = "cannot listen on " + address + ":" + port + ": " + std::strerror(lastError);
            return -1;
        }

        sockaddr_storage bound{};
        socklen_t boundLength = sizeof(bound);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0) {
            error = "cannot inspect listening socket: " + std::string(std::strerror(errno));
            closeFd(listener);
            return -1;
        }
        if (bound.ss_family == AF_INET)
            actualPort = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
        else
            actualPort = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
        return listener;
    }

    bool initialize(std::string& error) {
        if (!config.enableAvr && !config.enableBeast) {
            error = "no TCP output protocol enabled";
            return false;
        }
        int pipeFds[2];
        if (::pipe(pipeFds) != 0) {
            error = "cannot create TCP control pipe: " + std::string(std::strerror(errno));
            return false;
        }
        controlRead = pipeFds[0];
        controlWrite = pipeFds[1];
        if (!setNonBlockingCloseOnExec(controlRead) || !setNonBlockingCloseOnExec(controlWrite)) {
            error = "cannot configure TCP control pipe: " + std::string(std::strerror(errno));
            cleanup();
            return false;
        }
        if (config.enableAvr) {
            avrListener = createListener(config.bindAddress, config.avrPort, actualAvrPort, error);
            if (avrListener < 0) {
                cleanup();
                return false;
            }
        }
        if (config.enableBeast) {
            beastListener = createListener(config.bindAddress, config.beastPort, actualBeastPort, error);
            if (beastListener < 0) {
                cleanup();
                return false;
            }
        }
        return true;
    }

    void cleanup() noexcept {
        for (auto& client : clients)
            closeFd(client.fd);
        clients.clear();
        connectedClients.store(0, std::memory_order_relaxed);
        closeFd(avrListener);
        closeFd(beastListener);
        closeFd(controlRead);
        closeFd(controlWrite);
    }

    void acceptClients(int listener, Protocol protocol) {
        for (;;) {
            const int fd = ::accept(listener, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                return;
            }
            if (!setNonBlockingCloseOnExec(fd)) {
                ::close(fd);
                continue;
            }
            const int yes = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            if (config.acceptedSocketSendBuffer != 0) {
                const int bytes = static_cast<int>(std::min(config.acceptedSocketSendBuffer, size_t(INT_MAX)));
                ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
            }
#ifdef SO_NOSIGPIPE
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
            clients.push_back({fd, protocol, {}, 0});
            connectedClients.store(clients.size(), std::memory_order_relaxed);
        }
    }

    void appendFrame(Client& client, const EncodedModeSFrame& encoded) {
        const size_t remaining = client.pending.size() - client.offset;
        if (remaining + encoded.size > config.clientBufferLimit) {
            slowDisconnects.fetch_add(1, std::memory_order_relaxed);
            closeFd(client.fd);
            return;
        }
        if (client.offset != 0 && (client.offset == client.pending.size() || client.offset > 4096)) {
            client.pending.erase(client.pending.begin(), client.pending.begin() + client.offset);
            client.offset = 0;
        }
        client.pending.insert(client.pending.end(), encoded.bytes.begin(), encoded.bytes.begin() + encoded.size);
    }

    void distributeFrames() {
        ModeSFrame frame;
        while (frames.tryPop(frame)) {
            const bool haveAvr = std::any_of(clients.begin(), clients.end(),
                                             [](const Client& c) { return c.fd >= 0 && c.protocol == Protocol::Avr; });
            const bool haveBeast = std::any_of(clients.begin(), clients.end(), [](const Client& c) {
                return c.fd >= 0 && c.protocol == Protocol::Beast;
            });
            EncodedModeSFrame avr;
            EncodedModeSFrame beast;
            if (haveAvr)
                avr = encodeAvr(frame);
            if (haveBeast)
                beast = encodeBeast(frame);
            for (auto& client : clients) {
                if (client.fd < 0)
                    continue;
                if (client.protocol == Protocol::Avr && haveAvr)
                    appendFrame(client, avr);
                else if (client.protocol == Protocol::Beast && haveBeast)
                    appendFrame(client, beast);
            }
        }
    }

    void flushClient(Client& client) {
        while (client.fd >= 0 && client.offset < client.pending.size()) {
            const ssize_t sent = ::send(client.fd, client.pending.data() + client.offset,
                                        client.pending.size() - client.offset, sendFlags());
            if (sent > 0) {
                client.offset += size_t(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            closeFd(client.fd);
        }
        if (client.offset == client.pending.size()) {
            client.pending.clear();
            client.offset = 0;
        }
    }

    void removeClosedClients() {
        clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Client& client) {
                          return client.fd < 0;
                      }), clients.end());
        connectedClients.store(clients.size(), std::memory_order_relaxed);
    }

    void run() {
        while (isRunning.load(std::memory_order_acquire)) {
            std::vector<pollfd> events;
            events.reserve(3 + clients.size());
            events.push_back({controlRead, POLLIN, 0});
            if (avrListener >= 0)
                events.push_back({avrListener, POLLIN, 0});
            if (beastListener >= 0)
                events.push_back({beastListener, POLLIN, 0});
            for (const auto& client : clients) {
                short requested = POLLIN;
                if (client.offset < client.pending.size())
                    requested |= POLLOUT;
                events.push_back({client.fd, requested, 0});
            }

            int ready;
            do {
                ready = ::poll(events.data(), events.size(), 10);
            } while (ready < 0 && errno == EINTR && isRunning.load(std::memory_order_acquire));

            size_t index = 0;
            if (events[index++].revents & POLLIN) {
                std::array<char, 64> discard;
                while (::read(controlRead, discard.data(), discard.size()) > 0) {}
            }
            if (avrListener >= 0 && (events[index++].revents & POLLIN))
                acceptClients(avrListener, Protocol::Avr);
            if (beastListener >= 0 && (events[index++].revents & POLLIN))
                acceptClients(beastListener, Protocol::Beast);

            const size_t clientsAtPoll = events.size() - index;
            for (size_t i = 0; i < clientsAtPoll; ++i) {
                Client& client = clients[i];
                const short occurred = events[index + i].revents;
                if (occurred & (POLLERR | POLLHUP | POLLNVAL)) {
                    closeFd(client.fd);
                    continue;
                }
                if (occurred & POLLIN) {
                    char discard[64];
                    const ssize_t received = ::recv(client.fd, discard, sizeof(discard), 0);
                    if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                        closeFd(client.fd);
                }
            }

            distributeFrames();
            for (auto& client : clients)
                flushClient(client);
            removeClosedClients();
        }
        cleanup();
    }
};

TcpOutputServer::TcpOutputServer(TcpOutputConfig config) : m_impl(std::make_unique<Impl>(std::move(config))) {}
TcpOutputServer::~TcpOutputServer() { stop(); }

bool TcpOutputServer::start(std::string& error) {
    if (m_impl->isRunning.load(std::memory_order_relaxed)) {
        error = "TCP output server is already running";
        return false;
    }
    if (!m_impl->initialize(error))
        return false;
    m_impl->isRunning.store(true, std::memory_order_release);
    m_impl->worker = std::thread([impl = m_impl.get()] { impl->run(); });
    return true;
}

void TcpOutputServer::stop() noexcept {
    if (!m_impl->isRunning.exchange(false, std::memory_order_acq_rel)) {
        if (m_impl->worker.joinable())
            m_impl->worker.join();
        return;
    }
    const char wake = 1;
    (void)::write(m_impl->controlWrite, &wake, 1);
    if (m_impl->worker.joinable())
        m_impl->worker.join();
}

bool TcpOutputServer::tryPublish(const ModeSFrame& frame) noexcept {
    if (!m_impl->isRunning.load(std::memory_order_acquire))
        return false;
    if (m_impl->frames.tryPush(frame))
        return true;
    m_impl->dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool TcpOutputServer::running() const noexcept { return m_impl->isRunning.load(std::memory_order_acquire); }
uint16_t TcpOutputServer::avrPort() const noexcept { return m_impl->actualAvrPort; }
uint16_t TcpOutputServer::beastPort() const noexcept { return m_impl->actualBeastPort; }
size_t TcpOutputServer::clientCount() const noexcept { return m_impl->connectedClients.load(std::memory_order_relaxed); }
uint64_t TcpOutputServer::droppedFrames() const noexcept { return m_impl->dropped.load(std::memory_order_relaxed); }
uint64_t TcpOutputServer::slowClientDisconnects() const noexcept {
    return m_impl->slowDisconnects.load(std::memory_order_relaxed);
}
