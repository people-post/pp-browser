#pragma once

#include "foundation/sockets/TcpSockets.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace pbr::test {

/** A loopback HTTP server answering every request with 200 and the body set, one connection at a time. */
class LocalHttpServer {
public:
  LocalHttpServer() {
    auto listener = sockets::Listen("127.0.0.1", 0);
    if (!listener) {
      throw std::runtime_error("local HTTP server setup failed: " + listener.error().message);
    }
    listener_.Reset(listener->socket);
    port_ = listener->port;
    thread_ = std::thread([this] { Serve(); });
  }

  ~LocalHttpServer() {
    stopping_.store(true);
    // Wake the blocking accept.
    if (auto wake = sockets::ConnectLoopback(port_)) {
      sockets::Close(*wake);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  LocalHttpServer(const LocalHttpServer&) = delete;
  LocalHttpServer& operator=(const LocalHttpServer&) = delete;

  void SetResponse(std::string body) {
    std::lock_guard lock(response_mutex_);
    response_ = std::move(body);
  }

  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
  void Serve() {
    while (!stopping_.load()) {
      sockets::OwnedSocket socket(sockets::Accept(listener_.Get()));
      if (stopping_.load()) {
        return;
      }
      if (!socket.Valid()) {
        continue;
      }
      std::string request;
      if (!sockets::RecvUntil(socket.Get(), request, "\r\n\r\n")) {
        continue;
      }
      std::string body;
      {
        std::lock_guard lock(response_mutex_);
        body = response_;
      }
      sockets::SendAll(socket.Get(),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\n"
                           "Connection: close\r\n\r\n" +
                           body);
      sockets::ShutdownBoth(socket.Get());
    }
  }

  sockets::OwnedSocket listener_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex response_mutex_;
  std::string response_;
  unsigned short port_ = 0;
};

} // namespace pbr::test
