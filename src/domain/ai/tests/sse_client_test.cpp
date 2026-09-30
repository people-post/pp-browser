#include "common/PlatformLimits.h"
#include "domain/ai/LlmClient.h"
#include "domain/ai/SseClient.h"
#include "foundation/error/AppError.h"

#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <future>
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

pbr::SseRequest RequestFor(const SseTestServer& server) {
  pbr::SseRequest request;
  request.url = server.Url() + "/v1/stream";
  request.json_body = R"({"stream":true})";
  return request;
}

std::string HttpError(int status, const std::string& reason, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: application/json\r\n"
         "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

void ExpectSameErrorAsLlmClient(int status, const std::string& reason, const std::string& body) {
  SseTestServer server({{HttpError(status, reason, body)}}, false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  auto result = client.Post(RequestFor(server), [](const pbr::SseEvent&) {}, cancel);
  ASSERT_FALSE(static_cast<bool>(result));
  const pbr::Error expected = pbr::LlmClient::MapHttpError(status, body);
  EXPECT_EQ(result.error().category, expected.category);
  EXPECT_EQ(result.error().code, expected.code);
}

} // namespace

TEST(SseClientTest, DeliversEventsInOrderAcrossDelayedChunks) {
  SseTestServer server({{kSseHead},
                        {"data: one\n\nevent: delta\nda", 30},
                        {"ta: two\n\n", 30},
                        {"data: \xE4\xBD", 30}, // "你" split mid-character
                        {"\xA0\n\n", 30}},
                       false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  std::vector<pbr::SseEvent> events;
  auto result = client.Post(RequestFor(server), [&](const pbr::SseEvent& e) { events.push_back(e); }, cancel);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, pbr::SseOutcome::Completed);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "one");
  EXPECT_EQ(events[1].event, "delta");
  EXPECT_EQ(events[1].data, "two");
  EXPECT_EQ(events[2].data, "你");
}

TEST(SseClientTest, SendsBearerAcceptAndBody) {
  SseTestServer server({{kSseHead}, {"data: x\n\n"}}, false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  pbr::SseRequest request = RequestFor(server);
  request.bearer_token = "secret-token";
  ASSERT_TRUE(static_cast<bool>(client.Post(request, [](const pbr::SseEvent&) {}, cancel)));
  ASSERT_TRUE(server.WaitForRequest());
  const std::string seen = server.Request();
  EXPECT_NE(seen.find("POST /v1/stream"), std::string::npos);
  EXPECT_NE(seen.find("Authorization: Bearer secret-token"), std::string::npos);
  EXPECT_NE(seen.find("Accept: text/event-stream"), std::string::npos);
  EXPECT_NE(seen.find("Content-Type: application/json"), std::string::npos);
  EXPECT_NE(seen.find(R"({"stream":true})"), std::string::npos);
}

TEST(SseClientTest, NoAuthorizationHeaderWithoutToken) {
  SseTestServer server({{kSseHead}}, false);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  ASSERT_TRUE(static_cast<bool>(client.Post(RequestFor(server), [](const pbr::SseEvent&) {}, cancel)));
  ASSERT_TRUE(server.WaitForRequest());
  EXPECT_EQ(server.Request().find("Authorization"), std::string::npos);
}

TEST(SseClientTest, CancelWhileServerIsSilentReturnsCancelledQuickly) {
  SseTestServer server({{kSseHead}, {"data: first\n\n"}}, true);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  std::atomic<bool> got_first{false};
  std::atomic<std::chrono::steady_clock::rep> finished_at{0};

  auto worker = std::async(std::launch::async, [&] {
    auto result = client.Post(RequestFor(server), [&](const pbr::SseEvent&) { got_first = true; }, cancel);
    finished_at = std::chrono::steady_clock::now().time_since_epoch().count();
    return result;
  });

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!got_first && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(got_first);
  const auto cancelled_at = std::chrono::steady_clock::now();
  cancel = true;

  ASSERT_EQ(worker.wait_for(10s), std::future_status::ready);
  auto result = worker.get();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, pbr::SseOutcome::Cancelled);
  const auto elapsed = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(finished_at.load())) - cancelled_at;
  EXPECT_LT(elapsed, 1s);
  EXPECT_TRUE(server.WaitForDisconnect());
}

TEST(SseClientTest, CancelBeforeAnyResponseReturnsCancelled) {
  SseTestServer server({}, true); // accepts and reads the request, never answers
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  auto worker = std::async(std::launch::async, [&] { return client.Post(RequestFor(server), [](const pbr::SseEvent&) {}, cancel); });
  ASSERT_TRUE(server.WaitForRequest());
  cancel = true;
  ASSERT_EQ(worker.wait_for(10s), std::future_status::ready);
  auto result = worker.get();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, pbr::SseOutcome::Cancelled);
  EXPECT_TRUE(server.WaitForDisconnect());
}

TEST(SseClientTest, IdleTimeoutReturnsTimeoutError) {
  SseTestServer server({{kSseHead}, {"data: only\n\n"}}, true);
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  pbr::SseRequest request = RequestFor(server);
  request.idle_timeout_s = 1;
  std::vector<pbr::SseEvent> events;
  auto result = client.Post(request, [&](const pbr::SseEvent& e) { events.push_back(e); }, cancel);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().category, static_cast<int32_t>(pbr::ErrorCategory::Network));
  EXPECT_EQ(result.error().code, static_cast<int32_t>(pbr::Err::Network::Timeout));
  EXPECT_EQ(events.size(), 1u); // events delivered before the stall stay delivered
}

TEST(SseClientTest, RateLimitMapsLikeLlmClient) {
  ExpectSameErrorAsLlmClient(429, "Too Many Requests", R"({"error":{"message":"slow down"}})");
}

TEST(SseClientTest, UnauthorizedMapsLikeLlmClient) {
  ExpectSameErrorAsLlmClient(401, "Unauthorized", R"({"detail":{"error":{"message":"bad key","code":"auth_error"}}})");
}

TEST(SseClientTest, ConnectionRefusedIsUnreachable) {
  std::string url;
  {
    SseTestServer server({}, false);
    url = server.Url();
  } // server gone: nothing listens on that port any more
  pbr::SseClient client;
  std::atomic<bool> cancel{false};
  pbr::SseRequest request;
  request.url = url;
  request.json_body = "{}";
  auto result = client.Post(request, [](const pbr::SseEvent&) {}, cancel);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().category, static_cast<int32_t>(pbr::ErrorCategory::Network));
  EXPECT_EQ(result.error().code, static_cast<int32_t>(pbr::Err::Network::Unreachable));
}
