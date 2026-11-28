#pragma once

#include "foundation/sockets/TcpSockets.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pbr::test {

/**
 * A loopback HTTP/1.1 server whose answers a test scripts per request (status, headers such as `Location`,
 * body), one connection at a time, each closed after its answer. It records what it was asked and how many
 * connections it accepted (a refused redirect hop never connects).
 */
class ScriptedHttpServer {
public:
  struct Request {
    std::string method;
    /** Path and query, as sent (`/a/b?t=x`). */
    std::string target;
    std::string body;
    /** The request's Content-Length header (0 when absent). */
    uint64_t content_length = 0;
    /** The raw request line and headers, up to the blank line. */
    std::string head;
  };
  struct Reply {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
  };
  using Handler = std::function<Reply(const Request&)>;

  /** `port` 0: any free port; else that one (a test that brings a server "back" where none answered). */
  explicit ScriptedHttpServer(const unsigned short port = 0) {
    auto listener = sockets::Listen("127.0.0.1", port);
    if (!listener) {
      throw std::runtime_error("scripted HTTP server: " + listener.error().message);
    }
    listener_.Reset(listener->socket);
    port_ = listener->port;
    thread_ = std::thread([this] { Serve(); });
  }
  ~ScriptedHttpServer() {
    stopping_.store(true);
    // Wake a blocking accept, then close.
    if (auto wake = sockets::ConnectLoopback(port_)) {
      sockets::Close(*wake);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  ScriptedHttpServer(const ScriptedHttpServer&) = delete;
  ScriptedHttpServer& operator=(const ScriptedHttpServer&) = delete;

  void SetHandler(Handler handler) {
    std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
  }
  /** `http://127.0.0.1:<port>` (no trailing slash). */
  std::string Origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
  unsigned short Port() const { return port_; }

  std::vector<Request> Requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
  }
  int Connections() const { return connections_.load(); }

private:
  void Serve() {
    while (!stopping_.load()) {
      sockets::OwnedSocket socket(sockets::Accept(listener_.Get()));
      if (!socket.Valid() || stopping_.load()) {
        return;
      }
      connections_.fetch_add(1);
      Handle(socket.Get());
    }
  }

  static std::string HeaderValue(const std::string& head, const std::string& name) {
    const std::string needle = "\r\n" + name + ": ";
    const auto at = head.find(needle);
    if (at == std::string::npos) {
      return {};
    }
    const auto start = at + needle.size();
    return head.substr(start, head.find("\r\n", start) - start);
  }

  void Handle(const sockets::Socket socket) {
    std::string buf;
    if (!sockets::RecvUntil(socket, buf, "\r\n\r\n")) {
      return;
    }
    const size_t head_size = buf.find("\r\n\r\n") + 4;
    const std::string head = buf.substr(0, head_size);
    Request request;
    const auto method_end = head.find(' ');
    const auto target_end = head.find(' ', method_end + 1);
    if (method_end == std::string::npos || target_end == std::string::npos) {
      return;
    }
    request.head = head;
    request.method = head.substr(0, method_end);
    request.target = head.substr(method_end + 1, target_end - method_end - 1);
    const std::string length_text = HeaderValue(head, "Content-Length");
    const uint64_t length = length_text.empty() ? 0 : std::stoull(length_text);
    request.content_length = length;
    bool ok = true;
    if (length > 0 && HeaderValue(head, "Expect") == "100-continue") {
      ok = sockets::SendAll(socket, "HTTP/1.1 100 Continue\r\n\r\n");
    }
    request.body = buf.substr(head_size);
    while (ok && request.body.size() < length) {
      char chunk[16 * 1024];
      const long n = sockets::Recv(socket, chunk, sizeof(chunk));
      if (n <= 0) {
        break;
      }
      request.body.append(chunk, static_cast<size_t>(n));
    }
    Handler handler;
    {
      std::lock_guard lock(mutex_);
      requests_.push_back(request);
      handler = handler_;
    }
    const Reply reply = handler ? handler(request) : Reply{.status = 404, .headers = {}, .body = ""};
    std::string text = "HTTP/1.1 " + std::to_string(reply.status) +
                       " X\r\nConnection: close\r\nContent-Length: " + std::to_string(reply.body.size()) + "\r\n";
    for (const auto& [name, value] : reply.headers) {
      text += name + ": " + value + "\r\n";
    }
    text += "\r\n" + reply.body;
    sockets::SendAll(socket, text);
    sockets::ShutdownBoth(socket);
  }

  sockets::OwnedSocket listener_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<int> connections_{0};
  unsigned short port_ = 0;
  mutable std::mutex mutex_;
  Handler handler_;
  std::vector<Request> requests_;
};

} // namespace pbr::test
