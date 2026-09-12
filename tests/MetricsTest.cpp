/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Martin Gronemann
 *
 * This file is part of stream1090 and is licensed under the GNU General
 * Public License v3.0. See the top-level LICENSE file for details.
 */

#include "Metrics.hpp"
#include "MetricsServer.hpp"

#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Minimal client: one request, read until the peer closes.
bool fetch(uint16_t port, const char* path, std::string& response) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    timeval timeout{ 5, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }

    const std::string request = std::string("GET ") + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
    if (::send(fd, request.data(), request.size(), 0) < 0) {
        ::close(fd);
        return false;
    }

    response.clear();
    char buffer[4096];
    for (;;) {
        const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
            break;
        response.append(buffer, size_t(n));
    }

    ::close(fd);
    return !response.empty();
}

} // namespace

int main() {
    // ---- the window folds into a monotonic total --------------------------
    Stats::StatsLog log;
    log.log(Stats::DF17_GOOD_MESSAGE);
    log.logSent(17);
    log.reset(); // what printTick does every few seconds
    log.log(Stats::DF17_GOOD_MESSAGE);
    log.logSent(17);

    const auto cumulative = log.cumulative();
    if (cumulative.events[Stats::DF17_GOOD_MESSAGE] != 2) return 1;
    if (cumulative.sent[17] != 2) return 2;
    // the window itself starts over, which is what the stderr table wants
    if (log.getCount(Stats::DF17_GOOD_MESSAGE) != 1) return 3;
    if (log.getSent(17) != 1) return 4;

    // ---- rendering --------------------------------------------------------
    auto& reg = Metrics::registry();
    reg.setBuildInfo("test", "passthrough", 2400000, 8000000, 8);
    reg.setDeviceName("rtlsdr");
    reg.deviceUp.set(1.0);
    reg.outputBytes.inc(128);
    reg.tcpFramesDropped.inc(3);

    Stats::Counters counters{};
    counters.events[Stats::DF17_GOOD_MESSAGE] = 7;
    counters.events[Stats::DF17_REPAIR_FAILED] = 3;
    counters.events[Stats::REJECT_ALTITUDE] = 2;
    counters.sent[17] = 5;
    counters.dups[17] = 1;
    reg.publishDemod(counters);

    const auto page = Metrics::render(reg);

    if (!contains(page, "# TYPE stream1090_build_info gauge")) return 5;
    if (!contains(page, "stream1090_build_info{version=\"test\"")) return 6;
    if (!contains(page, "stream1090_device_up 1")) return 7;
    if (!contains(page, "stream1090_es_frames_total{result=\"crc_ok\"} 7")) return 8;
    if (!contains(page, "stream1090_es_frames_total{result=\"repair_failed\"} 3")) return 9;
    if (!contains(page, "stream1090_frames_rejected_total{reason=\"altitude\"} 2")) return 10;
    if (!contains(page, "stream1090_messages_total{df=\"17\"} 5")) return 11;
    if (!contains(page, "stream1090_messages_duplicate_total{df=\"17\"} 1")) return 12;
    if (!contains(page, "stream1090_output_bytes_total 128")) return 13;
    if (!contains(page, "stream1090_tcp_frames_dropped_total 3")) return 14;
    if (!contains(page, "stream1090_demod_events_total{event=\"df17_good_message\"} 7")) return 15;
    if (!contains(page, "stream1090_log_messages_total{level=\"error\"}")) return 16;
    if (!contains(page, "process_start_time_seconds")) return 17;
    if (!contains(page, "# HELP stream1090_metrics_snapshot_age_seconds")) return 18;

    // ---- listen address parsing ------------------------------------------
    std::string host;
    uint16_t port = 0;
    if (!MetricsServer::parseBind("0.0.0.0:9110", host, port) || host != "0.0.0.0" || port != 9110)
        return 19;
    if (!MetricsServer::parseBind(":9111", host, port) || host != "127.0.0.1" || port != 9111)
        return 20;
    if (!MetricsServer::parseBind("9112", host, port) || host != "127.0.0.1" || port != 9112)
        return 21;
    if (MetricsServer::parseBind("127.0.0.1:70000", host, port)) return 22;
    if (MetricsServer::parseBind("127.0.0.1:abc", host, port)) return 23;

    // ---- serving ----------------------------------------------------------
    MetricsServer server;
    if (!server.start("127.0.0.1:0")) return 24;

    std::string response;
    if (!fetch(server.port(), "/metrics", response)) return 25;
    if (!contains(response, "200 OK")) return 26;
    if (!contains(response, "text/plain; version=0.0.4")) return 27;
    if (!contains(response, "stream1090_build_info")) return 28;

    if (!fetch(server.port(), "/healthz", response)) return 29;
    if (!contains(response, "200 OK")) return 30;

    if (!fetch(server.port(), "/nope", response)) return 31;
    if (!contains(response, "404")) return 32;

    // readiness needs a live device
    if (!fetch(server.port(), "/readyz", response)) return 33;
    if (!contains(response, "200 OK")) return 34;
    reg.deviceUp.set(0.0);
    if (!fetch(server.port(), "/readyz", response)) return 35;
    if (!contains(response, "503")) return 36;

    // scrapes are counted, including the ones above
    if (reg.scrapes.get() < 2) return 37;

    server.stop();
    server.stop(); // idempotent

    return 0;
}
