#include "foundation/sockets/TcpSockets.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "common/PbrCompat.h"

using namespace pbr;
using namespace pbr::sockets;

TEST(TcpSocketsTest, ALoopbackListenerTakesAConnectionBothWays) {
  auto listener = Listen("127.0.0.1", 0);
  ASSERT_TRUE(static_cast<bool>(listener)) << listener.error().message;
  OwnedSocket server_end;
  ASSERT_NE(listener->port, 0);
  EXPECT_EQ(listener->bound, "127.0.0.1:" + std::to_string(listener->port));
  std::thread accept([&] { server_end.Reset(Accept(listener->socket)); });
  auto client = ConnectLoopback(listener->port);
  ASSERT_TRUE(static_cast<bool>(client)) << client.error().message;
  OwnedSocket client_end(*client);
  accept.join();
  ASSERT_TRUE(server_end.Valid());

  ASSERT_TRUE(SendAll(client_end.Get(), "GET / HTTP/1.1\r\nHost: x\r\n\r\nrest"));
  std::string got;
  ASSERT_TRUE(RecvUntil(server_end.Get(), got, "\r\n\r\n"));
  EXPECT_EQ(got.rfind("GET / HTTP/1.1", 0), 0u);
  ASSERT_TRUE(SendAll(server_end.Get(), "pong"));
  ShutdownBoth(server_end.Get());
  std::string reply;
  char buf[16];
  long n;
  while ((n = Recv(client_end.Get(), buf, sizeof(buf))) > 0) {
    reply.append(buf, static_cast<size_t>(n));
  }
  EXPECT_EQ(reply, "pong");
  EXPECT_EQ(n, 0) << "the peer's end reads as 0";
  Close(listener->socket);
}

TEST(TcpSocketsTest, PollSaysWhenAConnectionIsWaitingAndANonBlockingReadWouldBlock) {
  auto listener = Listen("127.0.0.1", 0);
  ASSERT_TRUE(static_cast<bool>(listener));
  std::vector<PollItem> items{{.socket = listener->socket, .want_read = true}};
  EXPECT_EQ(Poll(items, 0), 0);
  auto client = ConnectLoopback(listener->port);
  ASSERT_TRUE(static_cast<bool>(client));
  OwnedSocket client_end(*client);
  EXPECT_EQ(Poll(items, 2000), 1);
  EXPECT_TRUE(items[0].readable) << "a connection is waiting";
  OwnedSocket server_end(Accept(listener->socket));
  ASSERT_TRUE(server_end.Valid());
  ASSERT_TRUE(SetNonBlocking(server_end.Get(), true));
  char buf[8];
  EXPECT_LT(Recv(server_end.Get(), buf, sizeof(buf)), 0);
  EXPECT_TRUE(WouldBlock());
  Close(listener->socket);
}

TEST(TcpSocketsTest, IPv6LoopbackAndBadAddresses) {
  if (auto v6 = Listen("::1", 0)) {
    EXPECT_EQ(v6->bound, "::1:" + std::to_string(v6->port));
    Close(v6->socket);
  }
  auto bad = Listen("localhost", 0);
  ASSERT_FALSE(static_cast<bool>(bad)) << "numeric addresses only";
  EXPECT_NE(bad.error().message.find("not a numeric address"), std::string::npos);
}
