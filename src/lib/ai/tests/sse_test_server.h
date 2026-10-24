#pragma once

// Scripted loopback HTTP/SSE server shared by the SSE client tests.

#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {


using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

const char* const kSseHead = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/event-stream\r\n"
                             "Cache-Control: no-cache\r\n"
                             "Connection: close\r\n\r\n";

struct Step {
  std::string bytes;
  int delay_ms = 0; // wait before sending
};

// One-connection-at-a-time scripted server: reads the request, sends the steps, then closes
// (or holds the connection open). Records the request and whether the client disconnected.
class SseTestServer {
public:
  SseTestServer(std::vector<Step> steps, bool hold_open) : steps_(std::move(steps)), hold_open_(hold_open), acceptor_(io_) {
    const tcp::endpoint endpoint(asio::ip::address_v4::loopback(), 0);
    acceptor_.open(endpoint.protocol());
    acceptor_.bind(endpoint);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    Accept();
    thread_ = std::thread([this] { io_.run(); });
  }

  ~SseTestServer() {
    io_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  bool WaitForRequest() { return Wait([this] { return !request_.empty(); }); }
  bool WaitForDisconnect() { return Wait([this] { return disconnected_; }); }

  std::string Request() {
    std::lock_guard lock(mutex_);
    return request_;
  }

private:
  struct Connection {
    explicit Connection(asio::io_context& io) : socket(io), timer(io) {}
    tcp::socket socket;
    asio::steady_timer timer;
    asio::streambuf buffer;
    std::array<char, 256> scratch{};
    size_t step = 0;
  };

  template <class Pred>
  bool Wait(Pred pred) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, 10s, pred);
  }

  void Accept() {
    auto conn = std::make_shared<Connection>(io_);
    acceptor_.async_accept(conn->socket, [this, conn](const std::error_code& ec) {
      if (!ec) {
        ReadHead(conn);
      }
    });
  }

  void ReadHead(const std::shared_ptr<Connection>& conn) {
    asio::async_read_until(conn->socket, conn->buffer, "\r\n\r\n",
                           [this, conn](const std::error_code& ec, size_t head_size) {
                             if (ec) {
                               return;
                             }
                             std::string all = Buffered(*conn);
                             std::string lower = all.substr(0, head_size);
                             std::transform(lower.begin(), lower.end(), lower.begin(),
                                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                             size_t need = 0;
                             const size_t at = lower.find("content-length:");
                             if (at != std::string::npos) {
                               need = std::stoul(lower.substr(at + 15));
                             }
                             const size_t have = all.size() - head_size;
                             if (need > have) {
                               asio::async_read(conn->socket, conn->buffer, asio::transfer_exactly(need - have),
                                                [this, conn](const std::error_code& read_ec, size_t) {
                                                  if (!read_ec) {
                                                    Begin(conn);
                                                  }
                                                });
                             } else {
                               Begin(conn);
                             }
                           });
  }

  static std::string Buffered(Connection& conn) {
    return std::string(asio::buffers_begin(conn.buffer.data()), asio::buffers_end(conn.buffer.data()));
  }

  void Begin(const std::shared_ptr<Connection>& conn) {
    {
      std::lock_guard lock(mutex_);
      request_ = Buffered(*conn);
    }
    cv_.notify_all();
    Watch(conn);
    SendNext(conn);
  }

  // The client sends nothing after the request, so any read completion means it went away.
  void Watch(const std::shared_ptr<Connection>& conn) {
    conn->socket.async_read_some(asio::buffer(conn->scratch), [this, conn](const std::error_code& ec, size_t) {
      if (ec) {
        {
          std::lock_guard lock(mutex_);
          disconnected_ = true;
        }
        cv_.notify_all();
        return;
      }
      Watch(conn);
    });
  }

  void SendNext(const std::shared_ptr<Connection>& conn) {
    if (conn->step >= steps_.size()) {
      if (!hold_open_) {
        std::error_code ignored;
        conn->socket.shutdown(tcp::socket::shutdown_both, ignored);
        conn->socket.close(ignored);
      }
      return;
    }
    const Step& step = steps_[conn->step++];
    conn->timer.expires_after(std::chrono::milliseconds(step.delay_ms));
    conn->timer.async_wait([this, conn, &step](const std::error_code& ec) {
      if (ec) {
        return;
      }
      asio::async_write(conn->socket, asio::buffer(step.bytes), [this, conn](const std::error_code& write_ec, size_t) {
        if (!write_ec) {
          SendNext(conn);
        }
      });
    });
  }

  std::vector<Step> steps_;
  bool hold_open_;
  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::thread thread_;
  unsigned short port_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::string request_;
  bool disconnected_ = false;
};

std::string HttpError(int status, const std::string& reason, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: application/json\r\n"
         "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

} // namespace
