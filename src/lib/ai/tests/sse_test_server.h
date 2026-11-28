#pragma once

// Scripted loopback HTTP/SSE server shared by the SSE client tests.

#include "foundation/sockets/TcpSockets.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace sockets = pbr::sockets;

const char* const kSseHead =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n\r\n";

struct Step {
  std::string bytes;
  int delay_ms = 0; // wait before sending
};

// One-connection scripted server: reads the request, sends the steps, then closes (or holds the connection open).
// Records the request and whether the client disconnected.
class SseTestServer {
public:
  SseTestServer(std::vector<Step> steps, bool hold_open) : steps_(std::move(steps)), hold_open_(hold_open) {
    auto listener = sockets::Listen("127.0.0.1", 0);
    if (!listener) {
      throw std::runtime_error("SSE test server: " + listener.error().message);
    }
    listener_.Reset(listener->socket);
    port_ = listener->port;
    thread_ = std::thread([this] { Serve(); });
  }

  ~SseTestServer() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (!accepted_.load()) {
      if (auto wake = sockets::ConnectLoopback(port_)) {
        sockets::Close(*wake);
      }
    }
    {
      std::lock_guard lock(socket_mutex_);
      if (connection_.Valid()) {
        sockets::ShutdownBoth(connection_.Get());
      }
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (watcher_.joinable()) {
      watcher_.join();
    }
  }

  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  bool WaitForRequest() {
    return Wait([this] { return !request_.empty(); });
  }
  bool WaitForDisconnect() {
    return Wait([this] { return disconnected_; });
  }

  std::string Request() {
    std::lock_guard lock(mutex_);
    return request_;
  }

private:
  template <class Pred>
  bool Wait(Pred pred) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, 10s, pred);
  }

  void Serve() {
    sockets::OwnedSocket socket(sockets::Accept(listener_.Get()));
    accepted_.store(true);
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || !socket.Valid()) {
        return;
      }
    }
    const sockets::Socket raw = socket.Get();
    {
      std::lock_guard lock(socket_mutex_);
      connection_ = std::move(socket);
    }
    std::string all;
    if (!sockets::RecvUntil(raw, all, "\r\n\r\n")) {
      return;
    }
    const size_t head_size = all.find("\r\n\r\n") + 4;
    std::string lower = all.substr(0, head_size);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t need = 0;
    const size_t at = lower.find("content-length:");
    if (at != std::string::npos) {
      need = std::stoul(lower.substr(at + 15));
    }
    while (all.size() - head_size < need) {
      char chunk[4096];
      const long n = sockets::Recv(raw, chunk, sizeof(chunk));
      if (n <= 0) {
        return;
      }
      all.append(chunk, static_cast<size_t>(n));
    }
    {
      std::lock_guard lock(mutex_);
      request_ = all;
    }
    cv_.notify_all();
    // The client sends nothing after the request, so the read's end means it went away (or we closed).
    watcher_ = std::thread([this, raw] {
      char scratch[256];
      while (sockets::Recv(raw, scratch, sizeof(scratch)) > 0) {
      }
      {
        std::lock_guard lock(mutex_);
        disconnected_ = true;
      }
      cv_.notify_all();
    });
    for (const Step& step : steps_) {
      {
        std::unique_lock lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(step.delay_ms), [this] { return stopping_; })) {
          return;
        }
      }
      if (!sockets::SendAll(raw, step.bytes)) {
        return;
      }
    }
    if (!hold_open_) {
      sockets::ShutdownBoth(raw);
    }
  }

  std::vector<Step> steps_;
  bool hold_open_;
  sockets::OwnedSocket listener_;
  std::mutex socket_mutex_;
  sockets::OwnedSocket connection_;
  std::thread thread_;
  std::thread watcher_;
  std::atomic<bool> accepted_{false};
  unsigned short port_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::string request_;
  bool disconnected_ = false;
};

std::string HttpError(int status, const std::string& reason, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason +
         "\r\nContent-Type: application/json\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

} // namespace
