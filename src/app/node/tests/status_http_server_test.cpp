#include "app/node/StatusHttpServer.h"

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using tcp = asio::ip::tcp;

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

  std::unique_ptr<tcp::socket> Connect() {
    auto socket = std::make_unique<tcp::socket>(io_);
    socket->connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port_));
    return socket;
  }

  /** True when the server closed the connection (read hits EOF / reset) within `within`. */
  static bool ClosedWithin(tcp::socket& socket, std::chrono::milliseconds within) {
    socket.non_blocking(true);
    const auto until = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < until) {
      char byte = 0;
      std::error_code ec;
      socket.read_some(asio::buffer(&byte, 1), ec);
      if (ec && ec != asio::error::would_block) {
        return true; // eof / reset: closed
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  asio::io_context io_;
  StatusHttpServer server_;
  uint16_t port_ = 0;
};

// PB-027: a connection that sends no request is closed after the deadline, not held forever.
TEST_F(StatusHttpServerTest, AConnectionThatSendsNothingIsClosedAfterTheDeadline) {
  auto idle = Connect();
  EXPECT_FALSE(ClosedWithin(*idle, std::chrono::milliseconds(1000))) << "not before the deadline";
  EXPECT_TRUE(ClosedWithin(*idle, std::chrono::seconds(6)));
}

// PB-027: idle connections cannot take every descriptor: past the cap a new one is refused at once,
// and once they go the server answers again.
TEST_F(StatusHttpServerTest, PastTheCapANewConnectionIsRefusedAndTheServerRecovers) {
  std::vector<std::unique_ptr<tcp::socket>> held;
  for (int i = 0; i < 64; ++i) {
    held.push_back(Connect());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200)); // the server has accepted them
  auto over = Connect();
  EXPECT_TRUE(ClosedWithin(*over, std::chrono::milliseconds(1000)));

  held.clear();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto client = Connect();
  const std::string request = "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  asio::write(*client, asio::buffer(request));
  std::string response(64, '\0');
  std::error_code ec;
  const size_t n = client->read_some(asio::buffer(response), ec);
  ASSERT_GT(n, 0u) << ec.message();
  EXPECT_EQ(response.rfind("HTTP/1.1 200", 0), 0u) << response.substr(0, n);
}

} // namespace
} // namespace pbr
