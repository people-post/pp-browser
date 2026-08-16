#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/Libp2pScheduler.h"
#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/PeerSessionManager.h"
#include "libp2p/integration/host/StreamFrameIo.h"
#include "base/people/tests/libp2p_ephemeral_listen.h"

#include "common/WorkerPool.h"

#include <gtest/gtest.h>

#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

inline constexpr const char* kStreamFrameIoTestProtocol = "/pp-browser/stream-frame-io-test/1.0.0";

template <typename Result>
Result RunOnWorker(Libp2pHost& host, std::function<Result()> work) {
  std::promise<Result> promise;
  auto future = promise.get_future();
  PostLibp2pWorker(host, WorkerLane::Normal, [&] { promise.set_value(work()); });
  return future.get();
}

class StreamFrameIoTest : public ::testing::Test {
protected:
  void SetUp() override {
    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);

    auto a_started = test::StartEphemeralLoopbackHost(a_host_, a_port_);
    ASSERT_TRUE(a_started) << a_started.error().message;
    a_sessions_ = std::make_unique<PeerSessionManager>(a_host_, config);

    auto b_started = test::StartEphemeralLoopbackHost(b_host_, b_port_);
    ASSERT_TRUE(b_started) << b_started.error().message;
    b_sessions_ = std::make_unique<PeerSessionManager>(b_host_, config);

    auto a_id = a_host_.LocalPeerIdBase58();
    auto b_id = b_host_.LocalPeerIdBase58();
    ASSERT_TRUE(a_id);
    ASSERT_TRUE(b_id);
    b_ma_ = test::LoopbackP2pMultiaddr(b_port_, *b_id);
    a_ma_ = test::LoopbackP2pMultiaddr(a_port_, *a_id);
    ASSERT_TRUE(a_sessions_->RegisterEndpoint("b", b_ma_));
    ASSERT_TRUE(b_sessions_->RegisterEndpoint("a", a_ma_));
  }

  void TearDown() override {
    a_sessions_.reset();
    b_sessions_.reset();
    a_host_.Stop();
    b_host_.Stop();
  }

  int a_port_ = 0;
  int b_port_ = 0;
  std::string a_ma_;
  std::string b_ma_;
  Libp2pHost a_host_;
  Libp2pHost b_host_;
  std::unique_ptr<PeerSessionManager> a_sessions_;
  std::unique_ptr<PeerSessionManager> b_sessions_;
};

TEST(StreamFrameIoUnitTest, EncodeDecodeLengthPrefix) {
  const std::vector<uint8_t> body = {'h', 'e', 'l', 'l', 'o'};
  auto frame = EncodeLengthPrefixedFrame(body);
  ASSERT_EQ(frame.size(), 8u + body.size());
  std::vector<uint8_t> header(frame.begin(), frame.begin() + 8);
  EXPECT_EQ(DecodeLengthPrefixedHeader(header), body.size());
}

TEST_F(StreamFrameIoTest, BlockingReadWriteRoundTrip) {
  std::mutex mu;
  std::condition_variable cv;
  bool server_ready = false;
  std::shared_ptr<Stream> server_stream;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          {
            std::lock_guard lock(mu);
            server_stream = stream;
            server_ready = true;
          }
          cv.notify_one();
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  auto open_res = open_future.get();
  ASSERT_TRUE(open_res);
  auto client_stream = open_res.value().stream;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return server_ready; }));
  }

  const std::vector<uint8_t> payload = {'p', 'p'};
  auto write_result =
      RunOnWorker<Roe<void>>(a_host_, [&] { return BlockingWriteLengthPrefixedFrame(client_stream, payload); });
  ASSERT_TRUE(write_result) << write_result.error().message;

  auto read_result = RunOnWorker<Roe<std::vector<uint8_t>>>(
      b_host_, [&] { return BlockingReadLengthPrefixedFrame(server_stream); });
  ASSERT_TRUE(read_result) << read_result.error().message;
  EXPECT_EQ(*read_result, payload);
}

TEST_F(StreamFrameIoTest, AsyncReaderDeliverFrames) {
  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  std::vector<uint8_t> received;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          auto reader = std::make_shared<AsyncLengthPrefixedReader>();
          reader->Start(
              stream,
              [&](Roe<std::vector<uint8_t>> frame) {
                if (!frame) {
                  return;
                }
                {
                  std::lock_guard lock(mu);
                  received = *frame;
                  got = true;
                }
                cv.notify_one();
              },
              [] { return false; });
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  auto open_res = open_future.get();
  ASSERT_TRUE(open_res);
  auto client_stream = open_res.value().stream;

  const std::vector<uint8_t> payload = {'a', 's', 'y', 'n', 'c'};
  auto write_result =
      RunOnWorker<Roe<void>>(a_host_, [&] { return BlockingWriteLengthPrefixedFrame(client_stream, payload); });
  ASSERT_TRUE(write_result) << write_result.error().message;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }));
  }
  EXPECT_EQ(received, payload);
}

TEST_F(StreamFrameIoTest, StreamBridgeCopiesBytes) {
  std::mutex mu;
  std::condition_variable cv;
  bool bridge_ready = false;
  std::atomic<bool> bridge_cancelled{false};

  std::mutex reply_mu;
  std::condition_variable reply_cv;
  bool reply_got = false;
  bool reader_ready = false;
  std::vector<uint8_t> reply;

  a_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        a_host_.Post([&, stream = std::move(stream)]() mutable {
          auto reader = std::make_shared<AsyncLengthPrefixedReader>();
          reader->Start(
              stream,
              [&](Roe<std::vector<uint8_t>> frame) {
                if (!frame) {
                  return;
                }
                {
                  std::lock_guard lock(reply_mu);
                  reply = *frame;
                  reply_got = true;
                }
                reply_cv.notify_one();
              },
              [] { return false; });
          {
            std::lock_guard lock(reply_mu);
            reader_ready = true;
          }
          reply_cv.notify_one();
        });
      });

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol inbound_from_a) {
        auto inbound = std::move(inbound_from_a.stream);
        b_sessions_->OpenStream("a", {ProtocolName{kStreamFrameIoTestProtocol}},
                                [&, inbound = std::move(inbound)](
                                    outcome::result<libp2p::StreamAndProtocol> outbound_res) mutable {
                                  if (!outbound_res) {
                                    return;
                                  }
                                  auto outbound = outbound_res.value().stream;
                                  b_host_.Post(
                                      [&, inbound = std::move(inbound), outbound = std::move(outbound)]() mutable {
                                        auto bridge = std::make_shared<StreamBridge>();
                                        bridge->Start(
                                            inbound, outbound,
                                            [&] { return bridge_cancelled.load(std::memory_order_acquire); },
                                            [] {});
                                        {
                                          std::lock_guard lock(mu);
                                          bridge_ready = true;
                                        }
                                        cv.notify_one();
                                      });
                                });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  auto open_res = open_future.get();
  ASSERT_TRUE(open_res);
  auto client_stream = open_res.value().stream;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return bridge_ready; }));
  }
  {
    std::unique_lock lock(reply_mu);
    ASSERT_TRUE(reply_cv.wait_for(lock, std::chrono::seconds(3), [&] { return reader_ready; }));
  }

  const std::vector<uint8_t> payload = {'b', 'r', 'i', 'd', 'g', 'e'};
  auto write_result =
      RunOnWorker<Roe<void>>(a_host_, [&] { return BlockingWriteLengthPrefixedFrame(client_stream, payload); });
  ASSERT_TRUE(write_result) << write_result.error().message;

  {
    std::unique_lock lock(reply_mu);
    ASSERT_TRUE(reply_cv.wait_for(lock, std::chrono::seconds(3), [&] { return reply_got; }));
  }
  EXPECT_EQ(reply, payload);

  bridge_cancelled.store(true, std::memory_order_release);
}

TEST_F(StreamFrameIoTest, BlockingReadTimesOutAndResetsStream) {
  std::mutex mu;
  std::condition_variable cv;
  bool server_ready = false;
  std::shared_ptr<Stream> server_stream;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          {
            std::lock_guard lock(mu);
            server_stream = stream;
            server_ready = true;
          }
          cv.notify_one();
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  auto open_res = open_future.get();
  ASSERT_TRUE(open_res);
  auto client_stream = open_res.value().stream;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return server_ready; }));
  }

  // Declare a 1 KiB body (within max), then send nothing — reader must time out and reset.
  const std::vector<uint8_t> header_only = {0, 0, 0, 0, 0, 0, 0x04, 0x00}; // 1024

  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(client_stream, header_only,
                [&](outcome::result<void> result) { write_promise.set_value(result); });
  ASSERT_TRUE(write_future.get());

  LengthPrefixedFrameConfig config;
  config.max_frame_bytes = 256 * 1024;
  config.read_timeout = std::chrono::milliseconds(200);

  const auto started = std::chrono::steady_clock::now();
  auto read_result = RunOnWorker<Roe<std::vector<uint8_t>>>(
      b_host_, [&] { return BlockingReadLengthPrefixedFrame(server_stream, config); });
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_FALSE(read_result);
  EXPECT_NE(read_result.error().message.find("timed out"), std::string::npos)
      << read_result.error().message;
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST_F(StreamFrameIoTest, BlockingReadRejectsOversizedFrameWithoutReadingBody) {
  std::mutex mu;
  std::condition_variable cv;
  bool server_ready = false;
  std::shared_ptr<Stream> server_stream;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          {
            std::lock_guard lock(mu);
            server_stream = stream;
            server_ready = true;
          }
          cv.notify_one();
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  auto open_res = open_future.get();
  ASSERT_TRUE(open_res);
  auto client_stream = open_res.value().stream;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return server_ready; }));
  }

  // Length = max_frame_bytes + 1
  std::vector<uint8_t> header = {0, 0, 0, 0, 0, 0, 0x04, 0x01}; // 1025
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(client_stream, header, [&](outcome::result<void> result) { write_promise.set_value(result); });
  ASSERT_TRUE(write_future.get());

  LengthPrefixedFrameConfig config;
  config.max_frame_bytes = 1024;
  config.read_timeout = std::chrono::seconds(2);

  auto read_result = RunOnWorker<Roe<std::vector<uint8_t>>>(
      b_host_, [&] { return BlockingReadLengthPrefixedFrame(server_stream, config); });
  ASSERT_FALSE(read_result);
  EXPECT_NE(read_result.error().message.find("too large"), std::string::npos)
      << read_result.error().message;
}

TEST_F(StreamFrameIoTest, BlockingReadFailsWhenPeerClosesMidBody) {
  std::mutex mu;
  std::condition_variable cv;
  bool server_ready = false;
  std::shared_ptr<Stream> server_stream;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          {
            std::lock_guard lock(mu);
            server_stream = stream;
            server_ready = true;
          }
          cv.notify_one();
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  auto open_res = open_future.get();
  ASSERT_TRUE(open_res);
  auto client_stream = open_res.value().stream;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return server_ready; }));
  }

  // Length 8, send only 2 body bytes, then reset.
  std::vector<uint8_t> partial = {0, 0, 0, 0, 0, 0, 0, 8, 'a', 'b'};
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(client_stream, partial, [&](outcome::result<void> result) { write_promise.set_value(result); });
  ASSERT_TRUE(write_future.get());
  client_stream->reset();

  LengthPrefixedFrameConfig config;
  config.read_timeout = std::chrono::seconds(2);

  auto read_result = RunOnWorker<Roe<std::vector<uint8_t>>>(
      b_host_, [&] { return BlockingReadLengthPrefixedFrame(server_stream, config); });
  ASSERT_FALSE(read_result);
  EXPECT_TRUE(read_result.error().message.find("Failed to read") != std::string::npos ||
              read_result.error().message.find("timed out") != std::string::npos)
      << read_result.error().message;
}

TEST_F(StreamFrameIoTest, AsyncReaderTimesOutWithExecutor) {
  std::mutex mu;
  std::condition_variable cv;
  bool got_error = false;
  std::string error_message;
  std::shared_ptr<AsyncLengthPrefixedReader> kept_reader;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          LengthPrefixedFrameConfig config;
          config.read_timeout = std::chrono::milliseconds(200);
          config.timer_executor = b_host_.IoExecutor();
          auto reader = std::make_shared<AsyncLengthPrefixedReader>();
          {
            std::lock_guard lock(mu);
            kept_reader = reader;
          }
          reader->Start(
              stream,
              [&](Roe<std::vector<uint8_t>> frame) {
                if (frame) {
                  return;
                }
                {
                  std::lock_guard lock(mu);
                  error_message = frame.error().message;
                  got_error = true;
                }
                cv.notify_one();
              },
              [] { return false; }, std::move(config));
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  ASSERT_TRUE(open_future.get());

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got_error; }));
  }
  EXPECT_NE(error_message.find("timed out"), std::string::npos) << error_message;
  kept_reader.reset();
}

TEST_F(StreamFrameIoTest, DuplexSessionEchoesOnSameStream) {
  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  std::vector<uint8_t> received;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          auto duplex = std::make_shared<DuplexFrameSession>();
          duplex->Start(
              stream,
              [&, duplex](Roe<std::vector<uint8_t>> frame) {
                if (!frame) {
                  return false;
                }
                duplex->EnqueueOutbound(*frame);
                {
                  std::lock_guard lock(mu);
                  received = *frame;
                  got = true;
                }
                cv.notify_one();
                return true;
              },
              [] { return false; });
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  auto open_res = open_future.get();
  ASSERT_TRUE(open_res);
  auto client_stream = open_res.value().stream;

  const std::vector<uint8_t> payload = {'d', 'u', 'p', 'l', 'e', 'x'};
  auto write_result =
      RunOnWorker<Roe<void>>(a_host_, [&] { return BlockingWriteLengthPrefixedFrame(client_stream, payload); });
  ASSERT_TRUE(write_result) << write_result.error().message;

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }));
  }
  EXPECT_EQ(received, payload);

  auto echo = RunOnWorker<Roe<std::vector<uint8_t>>>(
      a_host_, [&] { return BlockingReadLengthPrefixedFrame(client_stream); });
  ASSERT_TRUE(echo) << echo.error().message;
  EXPECT_EQ(*echo, payload);
}

TEST_F(StreamFrameIoTest, DuplexWriteCallbackAndReadOnce) {
  std::mutex mu;
  std::condition_variable cv;
  bool got_req = false;
  bool wrote_ack = false;
  std::vector<uint8_t> request;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          auto duplex = std::make_shared<DuplexFrameSession>();
          auto policy = ControlJsonIoPolicy(b_host_.IoExecutor());
          duplex->Start(
              stream,
              [&, duplex](Roe<std::vector<uint8_t>> frame) {
                if (!frame) {
                  return false;
                }
                {
                  std::lock_guard lock(mu);
                  request = *frame;
                  got_req = true;
                }
                cv.notify_one();
                const std::vector<uint8_t> ack = {'o', 'k'};
                duplex->EnqueueOutbound(ack, [&, duplex](Roe<void> wrote) {
                  {
                    std::lock_guard lock(mu);
                    wrote_ack = static_cast<bool>(wrote);
                  }
                  cv.notify_one();
                  duplex->Stop();
                });
                return true;
              },
              [] { return false; }, std::move(policy));
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  auto open_res = open_future.get();
  ASSERT_TRUE(open_res);
  auto client_stream = open_res.value().stream;

  std::mutex client_mu;
  std::condition_variable client_cv;
  bool got_ack = false;
  std::vector<uint8_t> ack_body;
  a_host_.Post([&, client_stream]() {
    auto duplex = std::make_shared<DuplexFrameSession>();
    auto policy = ControlJsonIoPolicy(a_host_.IoExecutor());
    const std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};
    (void)duplex->EnqueueOutbound(payload);
    duplex->Start(
        client_stream,
        [&, duplex](Roe<std::vector<uint8_t>> frame) {
          if (!frame) {
            return false;
          }
          {
            std::lock_guard lock(client_mu);
            ack_body = *frame;
            got_ack = true;
          }
          client_cv.notify_one();
          return false;
        },
        [] { return false; }, std::move(policy));
  });

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got_req && wrote_ack; }));
  }
  EXPECT_EQ(request, (std::vector<uint8_t>{'p', 'i', 'n', 'g'}));
  EXPECT_TRUE(wrote_ack);

  {
    std::unique_lock lock(client_mu);
    ASSERT_TRUE(client_cv.wait_for(lock, std::chrono::seconds(3), [&] { return got_ack; }));
  }
  EXPECT_EQ(ack_body, (std::vector<uint8_t>{'o', 'k'}));
}

TEST_F(StreamFrameIoTest, DuplexDropOldestPolicy) {
  std::mutex mu;
  std::condition_variable cv;
  int drops = 0;
  size_t last_backlog = 0;

  b_host_.GetHost().setProtocolHandler(
      {ProtocolName{kStreamFrameIoTestProtocol}},
      [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        b_host_.Post([&, stream = std::move(stream)]() mutable {
          auto duplex = std::make_shared<DuplexFrameSession>();
          auto policy = CallMediaIoPolicy();
          policy.max_outbound_frames = 1;
          policy.on_outbound_drop = [&]() {
            std::lock_guard lock(mu);
            ++drops;
            cv.notify_one();
          };
          duplex->Start(
              stream,
              [](Roe<std::vector<uint8_t>>) { return true; },
              [] { return false; }, std::move(policy));
          duplex->EnqueueOutbound(std::vector<uint8_t>{1});
          duplex->EnqueueOutbound(std::vector<uint8_t>{2});
          duplex->EnqueueOutbound(std::vector<uint8_t>{3});
          {
            std::lock_guard lock(mu);
            last_backlog = duplex->OutboundBacklog();
          }
        });
      });

  std::promise<outcome::result<libp2p::StreamAndProtocol>> open_promise;
  auto open_future = open_promise.get_future();
  a_sessions_->OpenStream("b", {ProtocolName{kStreamFrameIoTestProtocol}},
                          [&](outcome::result<libp2p::StreamAndProtocol> result) {
                            open_promise.set_value(std::move(result));
                          });
  ASSERT_TRUE(open_future.get());

  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return drops >= 1; }));
  }
  EXPECT_GE(drops, 1);
  EXPECT_LE(last_backlog, 2u);
}

TEST(Libp2pSchedulerTest, PostsControlToWorkerPool) {
  Libp2pHost host;
  Libp2pHostConfig cfg;
  cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/0";
  ASSERT_TRUE(host.Start(cfg));

  Libp2pScheduler scheduler(host);
  std::atomic<bool> ran{false};
  scheduler.Post(Libp2pExecutor::ControlNormal, [&] { ran.store(true); });

  for (int i = 0; i < 100 && !ran.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(ran.load());
  host.Stop();
}

TEST(Libp2pSchedulerTest, PostComputeUsesOptionalPool) {
  WorkerPool compute_pool(1);

  Libp2pHost host;
  Libp2pHostConfig cfg;
  cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/0";
  ASSERT_TRUE(host.Start(cfg));

  Libp2pScheduler scheduler(host);
  EXPECT_FALSE(scheduler.PostCompute([] {}));

  scheduler.SetComputePool(&compute_pool);
  std::atomic<bool> ran{false};
  ASSERT_TRUE(scheduler.PostCompute([&] { ran.store(true); }));

  for (int i = 0; i < 100 && !ran.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(ran.load());

  host.Stop();
  compute_pool.Shutdown();
}

} // namespace
} // namespace pbr
