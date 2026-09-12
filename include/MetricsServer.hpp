/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#pragma once

#include "Metrics.hpp"
#include "Logger.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * A scrape endpoint, not a web server. One thread, one connection at a time,
 * three routes. A scrape renders in well under a millisecond and Prometheus
 * connects every few seconds, so there is nothing here worth making concurrent,
 * and a single blocking accept loop is the version that cannot go wrong.
 *
 * It listens on the loopback interface unless told otherwise: the endpoint has
 * no authentication of any kind, so putting it on a public interface is a
 * decision the operator has to make explicitly.
 */
class MetricsServer {
public:
    static constexpr uint16_t DefaultPort { 9109 };

    MetricsServer() = default;

    ~MetricsServer() { stop(); }

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    /// Accepts "addr:port", ":port" and a bare "port". An empty host means
    /// loopback.
    static bool parseBind(const std::string& spec, std::string& host, uint16_t& port) {
        const auto colon = spec.rfind(':');
        std::string portText;

        if (colon == std::string::npos) {
            host = "127.0.0.1";
            portText = spec;
        } else {
            host = spec.substr(0, colon);
            portText = spec.substr(colon + 1);
            if (host.empty())
                host = "127.0.0.1";
        }

        if (portText.empty()) {
            port = DefaultPort;
            return true;
        }

        // Port 0 is legal and means "let the kernel choose", which is how the
        // tests get a free port.
        char* end = nullptr;
        const auto value = std::strtol(portText.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || end == portText.c_str()
                || value < 0 || value > 65535)
            return false;

        port = uint16_t(value);
        return true;
    }

    bool start(const std::string& bindSpec) {
        std::string host;
        uint16_t port = DefaultPort;
        if (!parseBind(bindSpec, host, port)) {
            Log::error("Metrics", "Cannot parse the listen address: " + bindSpec);
            return false;
        }

        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen < 0) {
            Log::error("Metrics", std::string("socket() failed: ") + std::strerror(errno));
            return false;
        }

        int on = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            Log::error("Metrics", "Not an IPv4 address: " + host);
            closeListen();
            return false;
        }

        if (::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            Log::error("Metrics", "bind(" + host + ":" + std::to_string(port) + ") failed: "
                                  + std::strerror(errno));
            closeListen();
            return false;
        }

        if (::listen(m_listen, 8) != 0) {
            Log::error("Metrics", std::string("listen() failed: ") + std::strerror(errno));
            closeListen();
            return false;
        }

        // Port 0 asks the kernel to pick one, which the tests rely on.
        sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        if (::getsockname(m_listen, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
            m_port = ntohs(bound.sin_port);
        else
            m_port = port;

        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] { serve(); });
        Log::info("Metrics", "Serving /metrics on " + host + ":" + std::to_string(m_port));
        return true;
    }

    void stop() {
        if (!m_running.exchange(false, std::memory_order_acq_rel))
            return;

        // The accept loop wakes up on its own poll timeout and sees the flag.
        if (m_thread.joinable())
            m_thread.join();

        closeListen();
    }

    uint16_t port() const { return m_port; }

private:
    void closeListen() {
        if (m_listen >= 0) {
            ::close(m_listen);
            m_listen = -1;
        }
    }

    void serve() {
        while (m_running.load(std::memory_order_acquire)) {
            pollfd waiting{ m_listen, POLLIN, 0 };
            const int ready = ::poll(&waiting, 1, PollTimeoutMs);
            if (ready <= 0) {
                if (ready < 0 && errno != EINTR)
                    break;
                continue;
            }

            const int client = ::accept(m_listen, nullptr, nullptr);
            if (client < 0)
                continue;

            // A client that opens a connection and then goes quiet must not be
            // able to hold the only serving thread.
            timeval timeout{ 2, 0 };
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
            int on = 1;
            ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        std::string request;
        char buffer[1024];

        while (request.size() < MaxRequestBytes) {
            const auto n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0)
                return;

            request.append(buffer, size_t(n));
            if (request.find("\r\n\r\n") != std::string::npos
                    || request.find("\n\n") != std::string::npos)
                break;
        }

        const auto pathStart = request.find(' ');
        if (request.compare(0, 4, "GET ") != 0 || pathStart == std::string::npos) {
            respond(client, "405 Method Not Allowed", "text/plain", "method not allowed\n");
            return;
        }

        const auto pathEnd = request.find(' ', pathStart + 1);
        std::string path = request.substr(pathStart + 1,
                                          pathEnd == std::string::npos
                                              ? std::string::npos
                                              : pathEnd - pathStart - 1);
        const auto query = path.find('?');
        if (query != std::string::npos)
            path.erase(query);

        auto& reg = Metrics::registry();

        if (path == "/metrics") {
            respond(client, "200 OK", "text/plain; version=0.0.4; charset=utf-8",
                    Metrics::render(reg));
            return;
        }

        if (path == "/healthz") {
            respond(client, "200 OK", "text/plain", "ok\n");
            return;
        }

        if (path == "/readyz") {
            if (reg.ready())
                respond(client, "200 OK", "text/plain", "ready\n");
            else
                respond(client, "503 Service Unavailable", "text/plain", "not ready\n");
            return;
        }

        if (path == "/") {
            respond(client, "200 OK", "text/html",
                    "<html><head><title>stream1090</title></head><body>"
                    "<h1>stream1090</h1><ul>"
                    "<li><a href=\"/metrics\">/metrics</a></li>"
                    "<li><a href=\"/healthz\">/healthz</a></li>"
                    "<li><a href=\"/readyz\">/readyz</a></li>"
                    "</ul></body></html>\n");
            return;
        }

        respond(client, "404 Not Found", "text/plain", "not found\n");
    }

    void respond(int client, const char* status, const char* contentType,
                 const std::string& body) {
        std::string head = "HTTP/1.1 ";
        head += status;
        head += "\r\nContent-Type: ";
        head += contentType;
        head += "\r\nContent-Length: ";
        head += std::to_string(body.size());
        head += "\r\nConnection: close\r\n\r\n";

        if (sendAll(client, head))
            sendAll(client, body);
    }

    bool sendAll(int client, const std::string& data) {
        size_t offset = 0;
        while (offset < data.size()) {
#ifdef MSG_NOSIGNAL
            const auto n = ::send(client, data.data() + offset, data.size() - offset,
                                  MSG_NOSIGNAL);
#else
            const auto n = ::send(client, data.data() + offset, data.size() - offset, 0);
#endif
            if (n <= 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            offset += size_t(n);
        }
        return true;
    }

    static constexpr int PollTimeoutMs { 250 };
    static constexpr size_t MaxRequestBytes { 8192 };

    int m_listen { -1 };
    uint16_t m_port { 0 };
    std::atomic<bool> m_running { false };
    std::thread m_thread;
};
