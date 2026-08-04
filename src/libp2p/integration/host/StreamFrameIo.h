#pragma once

#include "common/Error.h"

#include <libp2p/connection/stream.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pbr {

struct LengthPrefixedFrameConfig {
  size_t max_frame_bytes = 256 * 1024;
  bool allow_empty_body = false;
};

std::vector<uint8_t> EncodeLengthPrefixedFrame(const std::vector<uint8_t>& body);
uint64_t DecodeLengthPrefixedHeader(const std::vector<uint8_t>& header8);

/** Blocking read/write for control-plane worker threads only. */
Roe<std::vector<uint8_t>> BlockingReadLengthPrefixedFrame(
    const std::shared_ptr<libp2p::connection::Stream>& stream,
    const LengthPrefixedFrameConfig& config = {});

Roe<void> BlockingWriteLengthPrefixedFrame(const std::shared_ptr<libp2p::connection::Stream>& stream,
                                           const std::vector<uint8_t>& body);

using StreamCancelCheck = std::function<bool()>;

/** Async length-prefixed frame reader — runs on host io_context thread. */
class AsyncLengthPrefixedReader : public std::enable_shared_from_this<AsyncLengthPrefixedReader> {
public:
  using FrameCallback = std::function<void(Roe<std::vector<uint8_t>>)>;

  void Start(std::shared_ptr<libp2p::connection::Stream> stream, FrameCallback on_frame,
             StreamCancelCheck is_cancelled, LengthPrefixedFrameConfig config = {});
  void Stop();

private:
  enum class Phase { Idle, Header, Body };

  void ReadHeader();
  void ReadBody(uint64_t payload_len);

  std::shared_ptr<libp2p::connection::Stream> stream_;
  FrameCallback on_frame_;
  StreamCancelCheck is_cancelled_;
  LengthPrefixedFrameConfig config_;
  std::atomic<bool> running_{false};
  Phase phase_ = Phase::Idle;
  libp2p::Bytes header_buf_;
  libp2p::Bytes payload_buf_;
};

/** Async length-prefixed frame writer with an outbound queue. */
class AsyncLengthPrefixedWriter : public std::enable_shared_from_this<AsyncLengthPrefixedWriter> {
public:
  using WriteCallback = std::function<void(Roe<void>)>;

  void Start(std::shared_ptr<libp2p::connection::Stream> stream);
  void Stop();
  bool Enqueue(const std::vector<uint8_t>& body, WriteCallback on_done = {});

private:
  void PumpWrite();

  std::shared_ptr<libp2p::connection::Stream> stream_;
  std::atomic<bool> running_{false};
  bool write_inflight_ = false;
  struct PendingWrite {
    std::shared_ptr<std::vector<uint8_t>> frame;
    WriteCallback on_done;
  };
  std::vector<PendingWrite> queue_;
};

/** Byte pump from one stream to another on the host io thread. */
class StreamBridge : public std::enable_shared_from_this<StreamBridge> {
public:
  static constexpr size_t kDefaultChunkBytes = 16 * 1024;

  void Start(std::shared_ptr<libp2p::connection::Stream> from,
             std::shared_ptr<libp2p::connection::Stream> to, StreamCancelCheck is_cancelled,
             std::function<void()> on_closed, size_t chunk_bytes = kDefaultChunkBytes);
  void Stop();

private:
  void PumpRead();

  std::shared_ptr<libp2p::connection::Stream> from_;
  std::shared_ptr<libp2p::connection::Stream> to_;
  StreamCancelCheck is_cancelled_;
  std::function<void()> on_closed_;
  size_t chunk_bytes_ = kDefaultChunkBytes;
  std::atomic<bool> running_{false};
  bool read_inflight_ = false;
  libp2p::Bytes chunk_buf_;
};

} // namespace pbr
