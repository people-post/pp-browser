#include "app/node/StatusHttpServer.h"
#include "foundation/sockets/TcpSockets.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

class StatusHttpServerTest : public ::testing::Test {
protected:
  void SetUp() override {
    StatusHttpBind bind;
    bind.host = "127.0.0.1";
    bind.port = 0;
    auto started = server_.Start(bind, StatusHttpAuthConfig{}, []() { return StatusHttpSnapshot{}; });
    ASSERT_TRUE(static_cast<bool>(started)) << started.error().message;
    const std::string bound = server_.BoundEndpoint();
    port_ = static_cast<uint16_t>(std::stoi(bound.substr(bound.rfind(':') + 1)));
  }
  void TearDown() override { server_.Stop(); }

  sockets::OwnedSocket Connect() {
    auto socket = sockets::ConnectLoopback(port_);
    EXPECT_TRUE(static_cast<bool>(socket)) << (socket ? "" : socket.error().message);
    return sockets::OwnedSocket(socket ? *socket : sockets::kInvalid);
  }

  /** True when the server closed the connection (a read hits its end or a reset) within `within`. */
  static bool ClosedWithin(const sockets::OwnedSocket& socket, std::chrono::milliseconds within) {
    sockets::SetNonBlocking(socket.Get(), true);
    const auto until = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < until) {
      char byte = 0;
      const long n = sockets::Recv(socket.Get(), &byte, 1);
      if (n == 0 || (n < 0 && !sockets::WouldBlock())) {
        return true; // its end / reset: closed
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  StatusHttpServer server_;
  uint16_t port_ = 0;
};

// PB-027: a connection that sends no request is closed after the deadline, not held forever.
TEST_F(StatusHttpServerTest, AConnectionThatSendsNothingIsClosedAfterTheDeadline) {
  auto idle = Connect();
  EXPECT_FALSE(ClosedWithin(idle, std::chrono::milliseconds(1000))) << "not before the deadline";
  EXPECT_TRUE(ClosedWithin(idle, std::chrono::seconds(6)));
}

// PB-027: idle connections cannot take every descriptor: past the cap a new one is refused at once,
// and once they go the server answers again.
TEST_F(StatusHttpServerTest, PastTheCapANewConnectionIsRefusedAndTheServerRecovers) {
  std::vector<sockets::OwnedSocket> held;
  for (int i = 0; i < 64; ++i) {
    held.push_back(Connect());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200)); // the server has accepted them
  auto over = Connect();
  EXPECT_TRUE(ClosedWithin(over, std::chrono::milliseconds(1000)));

  held.clear();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto client = Connect();
  ASSERT_TRUE(sockets::SendAll(client.Get(), "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  std::string response;
  ASSERT_TRUE(sockets::RecvUntil(client.Get(), response, "\r\n"));
  EXPECT_EQ(response.rfind("HTTP/1.1 200", 0), 0u) << response;
}

// The answer is written in full and the connection closed, the body after the head.
TEST_F(StatusHttpServerTest, AnAnswerIsWrittenWholeThenTheConnectionCloses) {
  auto client = Connect();
  ASSERT_TRUE(sockets::SendAll(client.Get(), "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  std::string response;
  char buf[4096];
  long n;
  while ((n = sockets::Recv(client.Get(), buf, sizeof(buf))) > 0) {
    response.append(buf, static_cast<size_t>(n));
  }
  EXPECT_EQ(n, 0) << "closed after the answer";
  EXPECT_EQ(response.rfind("HTTP/1.1 200", 0), 0u);
  const size_t body_at = response.find("\r\n\r\n");
  ASSERT_NE(body_at, std::string::npos);
  EXPECT_EQ(response.substr(body_at + 4, 1), "{");
}

TEST(StatusHttpServerBindTest, ANonNumericHostIsRefused) {
  StatusHttpServer server;
  StatusHttpBind bind;
  bind.host = "localhost";
  bind.port = 0;
  auto started = server.Start(bind, StatusHttpAuthConfig{}, []() { return StatusHttpSnapshot{}; });
  ASSERT_FALSE(static_cast<bool>(started));
  EXPECT_EQ(started.error().message, "invalid status HTTP bind host: localhost");
}

} // namespace
} // namespace pbr
