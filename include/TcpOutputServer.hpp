/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "ModeSFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct TcpOutputConfig {
    std::string bindAddress = "127.0.0.1";
    uint16_t avrPort = 0;
    uint16_t beastPort = 0;
    bool enableAvr = false;
    bool enableBeast = false;
    size_t clientBufferLimit = 256 * 1024;
    size_t acceptedSocketSendBuffer = 0;
};

class TcpOutputServer {
  public:
    explicit TcpOutputServer(TcpOutputConfig config);
    ~TcpOutputServer();

    TcpOutputServer(const TcpOutputServer&) = delete;
    TcpOutputServer& operator=(const TcpOutputServer&) = delete;

    bool start(std::string& error);
    void stop() noexcept;
    bool tryPublish(const ModeSFrame& frame) noexcept;

    bool running() const noexcept;
    uint16_t avrPort() const noexcept;
    uint16_t beastPort() const noexcept;
    size_t clientCount() const noexcept;
    uint64_t droppedFrames() const noexcept;
    uint64_t slowClientDisconnects() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
