#include "lib/net/HttpClient.h"

#include "foundation/sockets/TcpSockets.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace pbr {
namespace {

/** A tiny blocking HTTP server for one request at a time: PUT bodies are counted, GET answers are scripted. */
class FileTransferServer {
public:
  FileTransferServer() {
    auto listener = sockets::Listen("127.0.0.1", 0);
    EXPECT_TRUE(static_cast<bool>(listener));
    if (listener) {
      listener_.Reset(listener->socket);
      port_ = listener->port;
    }
    thread_ = std::thread([this] { Serve(); });
  }
  ~FileTransferServer() {
    stopping_.store(true);
    // Wake a blocking accept.
    if (auto wake = sockets::ConnectLoopback(port_)) {
      sockets::Close(*wake);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/blob"; }

  // Scripted GET answer.
  int get_status = 200;
  uint64_t get_body_bytes = 0;
  bool get_send_content_length = true; // false: the body is delimited by closing the connection
  int get_drip_ms = 0;                 // > 0: 10 bytes, then this long a pause, again and again
  // PUT result.
  int put_status = 200;
  std::atomic<uint64_t> put_received{0};
  std::atomic<uint64_t> put_declared{0};

private:
  void Serve() {
    while (!stopping_.load()) {
      sockets::OwnedSocket socket(sockets::Accept(listener_.Get()));
      if (!socket.Valid() || stopping_.load()) {
        return;
      }
      Handle(socket.Get());
    }
  }

  void Handle(const sockets::Socket socket) {
    std::string buf;
    if (!sockets::RecvUntil(socket, buf, "\r\n\r\n")) {
      return;
    }
    const size_t head_size = buf.find("\r\n\r\n") + 4;
    const std::string head = buf.substr(0, head_size);
    const bool is_put = head.rfind("PUT ", 0) == 0;
    bool ok = true;
    if (is_put) {
      uint64_t length = 0;
      const auto at = head.find("Content-Length: ");
      if (at != std::string::npos) {
        length = std::stoull(head.substr(at + 16));
      }
      put_declared = length;
      uint64_t received = buf.size() - head_size;
      std::string chunk(64 * 1024, '\0');
      while (received < length) {
        const long n = sockets::Recv(socket, chunk.data(), chunk.size());
        if (n <= 0) {
          break;
        }
        received += static_cast<uint64_t>(n);
      }
      put_received = received;
      sockets::SendAll(
          socket, "HTTP/1.1 " + std::to_string(put_status) + " X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    } else {
      std::string reply = "HTTP/1.1 " + std::to_string(get_status) + " X\r\nConnection: close\r\n";
      if (get_send_content_length) {
        reply += "Content-Length: " + std::to_string(get_body_bytes) + "\r\n";
      }
      reply += "\r\n";
      ok = sockets::SendAll(socket, reply);
      std::string chunk(64 * 1024, 'x');
      uint64_t left = get_body_bytes;
      while (left > 0 && ok) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(left, chunk.size()));
        if (get_drip_ms > 0) {
          n = std::min<size_t>(n, 10);
          std::this_thread::sleep_for(std::chrono::milliseconds(get_drip_ms));
        }
        ok = sockets::SendAll(socket, chunk.substr(0, n));
        left -= n;
      }
    }
    sockets::ShutdownBoth(socket);
  }

  sockets::OwnedSocket listener_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  unsigned short port_ = 0;
};

class HttpFileTransferTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() / ("pp_http_file_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string Write(const std::string& name, const uint64_t bytes) {
    const std::string path = (dir_ / name).string();
    std::ofstream out(path, std::ios::binary);
    const std::string chunk(1 << 16, 'u');
    for (uint64_t left = bytes; left > 0;) {
      const uint64_t n = std::min<uint64_t>(left, chunk.size());
      out.write(chunk.data(), static_cast<std::streamsize>(n));
      left -= n;
    }
    return path;
  }
  std::filesystem::path dir_;
  FileTransferServer server_;
};

TEST_F(HttpFileTransferTest, PutFileStreamsTheWholeFileAndReportsProgress) {
  const uint64_t size = 3 * 1024 * 1024 + 17;
  const std::string path = Write("up.bin", size);
  uint64_t last_done = 0;
  uint64_t last_total = 0;
  int calls = 0;
  auto response = HttpClient::PutFile(server_.Url(), path, {{"Content-Type", "application/octet-stream"}},
                                      [&](const uint64_t done, const uint64_t total) {
                                        last_done = done;
                                        last_total = total;
                                        ++calls;
                                        return true;
                                      });
  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(response->status_code, 200);
  EXPECT_EQ(server_.put_declared.load(), size);
  EXPECT_EQ(server_.put_received.load(), size);
  EXPECT_EQ(last_total, size);
  EXPECT_EQ(last_done, size);
  EXPECT_GT(calls, 1);
}

TEST_F(HttpFileTransferTest, PutFileCanBeCancelledFromTheProgressCallback) {
  const std::string path = Write("cancel.bin", 8 * 1024 * 1024);
  auto response = HttpClient::PutFile(server_.Url(), path, {}, [](uint64_t, uint64_t) { return false; });
  EXPECT_FALSE(response);
}

TEST_F(HttpFileTransferTest, PutFileOfAMissingFileFails) {
  EXPECT_FALSE(HttpClient::PutFile(server_.Url(), (dir_ / "missing").string()));
}

TEST_F(HttpFileTransferTest, GetToFileWritesWhatArrivesAndReportsProgress) {
  server_.get_body_bytes = 2 * 1024 * 1024 + 5;
  const std::string dest = (dir_ / "down.bin").string();
  uint64_t last_done = 0;
  auto result =
      HttpClient::GetToFile(server_.Url(), dest, server_.get_body_bytes + 16, [&](const uint64_t done, uint64_t) {
        last_done = done;
        return true;
      });
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result->status_code, 200);
  EXPECT_EQ(result->bytes, server_.get_body_bytes);
  EXPECT_EQ(std::filesystem::file_size(dest), server_.get_body_bytes);
  EXPECT_EQ(last_done, server_.get_body_bytes);
}

TEST_F(HttpFileTransferTest, GetToFileIsBiggerThan8MiBWithoutBuffering) {
  server_.get_body_bytes = 9 * 1024 * 1024; // above kMaxHttpClientBodyBytes, which a buffered GET refuses
  const std::string dest = (dir_ / "big.bin").string();
  auto result = HttpClient::GetToFile(server_.Url(), dest, server_.get_body_bytes + 16);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(std::filesystem::file_size(dest), server_.get_body_bytes);
}

TEST_F(HttpFileTransferTest, ABodyLongerThanTheCapIsAbortedAndTheFileDeleted) {
  server_.get_send_content_length = false; // no declared length: only the cap protects
  server_.get_body_bytes = 4 * 1024 * 1024;
  const std::string dest = (dir_ / "overrun.bin").string();
  auto result = HttpClient::GetToFile(server_.Url(), dest, 1024 * 1024);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("exceeds"), std::string::npos) << result.error().message;
  EXPECT_FALSE(std::filesystem::exists(dest)) << "an overrun leaves no temp file";
}

TEST_F(HttpFileTransferTest, ADeclaredLengthAboveTheCapIsRefusedBeforeReading) {
  server_.get_body_bytes = 4 * 1024 * 1024;
  const std::string dest = (dir_ / "declared.bin").string();
  auto result = HttpClient::GetToFile(server_.Url(), dest, 1024);
  ASSERT_FALSE(result);
  EXPECT_FALSE(std::filesystem::exists(dest));
}

TEST_F(HttpFileTransferTest, ANon2xxAnswerWritesNothing) {
  server_.get_status = 404;
  server_.get_body_bytes = 100;
  const std::string dest = (dir_ / "gone.bin").string();
  auto result = HttpClient::GetToFile(server_.Url(), dest, 1000);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status_code, 404);
  EXPECT_EQ(result->bytes, 0u);
  EXPECT_FALSE(std::filesystem::exists(dest));
}

// A server that trickles bytes must not hold the transfer: the speed floor aborts it (shortened for the test).
TEST_F(HttpFileTransferTest, ADrippingServerIsAbortedByTheSpeedLimitAndLeavesNothing) {
  HttpClient::SetStreamingStallLimitForTest(5000, 1); // under 5 KB/s for 1 s
  server_.get_body_bytes = 1024 * 1024;
  server_.get_drip_ms = 100; // 100 B/s
  const std::string dest = (dir_ / "drip.bin").string();
  const auto started = std::chrono::steady_clock::now();
  auto result = HttpClient::GetToFile(server_.Url(), dest, server_.get_body_bytes + 16);
  const auto took = std::chrono::steady_clock::now() - started;
  HttpClient::SetStreamingStallLimitForTest(0, 0);
  ASSERT_FALSE(result);
  EXPECT_LT(took, std::chrono::seconds(10)) << "aborted by the floor, not after a megabyte at 100 B/s";
  EXPECT_FALSE(std::filesystem::exists(dest));
}

TEST_F(HttpFileTransferTest, AFastUploadIsNotCaughtByTheSpeedFloor) {
  // A 3 s window: curl's Expect: 100-continue pause and a slow CI runner must not look like a stall.
  HttpClient::SetStreamingStallLimitForTest(5000, 3);
  const std::string path = Write("fast.bin", 2 * 1024 * 1024);
  auto response = HttpClient::PutFile(server_.Url(), path);
  HttpClient::SetStreamingStallLimitForTest(0, 0);
  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(server_.put_received.load(), 2u * 1024u * 1024u);
}

TEST_F(HttpFileTransferTest, GetToFileCanBeCancelledAndLeavesNothing) {
  server_.get_body_bytes = 6 * 1024 * 1024;
  const std::string dest = (dir_ / "cancelled.bin").string();
  auto result =
      HttpClient::GetToFile(server_.Url(), dest, server_.get_body_bytes + 16, [](uint64_t, uint64_t) { return false; });
  ASSERT_FALSE(result);
  EXPECT_FALSE(std::filesystem::exists(dest));
}

} // namespace
} // namespace pbr
