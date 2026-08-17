#pragma once

#include "common/Error.h"
#include "base/p2p/Libp2pExecutorLimits.h"

#include <libp2p/connection/stream.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pbr {

/** Default bound for control-plane JSON frames (chat, history, dial-back, circuit). */
inline constexpr std::chrono::milliseconds kDefaultControlFrameReadTimeout{8000};

struct LengthPrefixedFrameConfig {
  size_t max_frame_bytes = 256 * 1024;
  bool allow_empty_body = false;
  /**
   * Per-frame read deadline (header + body). Zero disables.
   * Blocking reads enforce this via wait_until + stream->reset().
   * Async / duplex need timer_executor set; otherwise the timeout is ignored.
   */
  std::chrono::milliseconds read_timeout{0};
  /** Host io_context executor — required for async/duplex read_timeout. */
  boost::asio::any_io_executor timer_executor{};
};

/** App-level QoS for one DuplexFrameSession (not Yamux / not the peer). */
enum class StreamIoClass {
  Realtime,
  Interactive,
  Control,
  Bulk,
};

struct StreamIoPolicy {
  enum class Drop { Never, Oldest };

  StreamIoClass cls = StreamIoClass::Control;
  Drop drop = Drop::Never;
  /** Queued frames (not counting the in-flight write). 0 = unbounded. */
  size_t max_outbound_frames = 0;
  bool write_preferred = false;
  /** Stop reading after the first successful frame (chat / history one-shot). */
  bool read_once = false;
  LengthPrefixedFrameConfig frame;
  std::function<void()> on_outbound_drop;
};

inline StreamIoPolicy CallMediaIoPolicy() {
  StreamIoPolicy policy;
  policy.cls = StreamIoClass::Realtime;
  policy.drop = StreamIoPolicy::Drop::Oldest;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxCallMediaOutboundFrames;
  policy.write_preferred = true;
  policy.frame.max_frame_bytes = Libp2pExecutorLimits::kMaxCallMediaFrameBytes;
  return policy;
}

inline StreamIoPolicy MediaRelayHopIoPolicy() {
  StreamIoPolicy policy;
  policy.cls = StreamIoClass::Realtime;
  policy.drop = StreamIoPolicy::Drop::Oldest;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxMediaRelayOutboundFrames;
  policy.write_preferred = true;
  policy.frame.max_frame_bytes = Libp2pExecutorLimits::kMaxMediaDataFrameBytes;
  policy.frame.allow_empty_body = true;
  return policy;
}

inline StreamIoPolicy MediaRelayClientIoPolicy() {
  StreamIoPolicy policy = MediaRelayHopIoPolicy();
  policy.cls = StreamIoClass::Interactive;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxMediaRelayClientOutboundFrames;
  return policy;
}

inline StreamIoPolicy ControlJsonIoPolicy(
    boost::asio::any_io_executor timer_executor,
    std::chrono::milliseconds read_timeout = kDefaultControlFrameReadTimeout,
    size_t max_frame_bytes = Libp2pExecutorLimits::kMaxChatStreamJsonBytes) {
  StreamIoPolicy policy;
  policy.cls = StreamIoClass::Control;
  policy.drop = StreamIoPolicy::Drop::Never;
  policy.max_outbound_frames = Libp2pExecutorLimits::kMaxControlOutboundFrames;
  policy.read_once = true;
  policy.frame.max_frame_bytes = max_frame_bytes;
  policy.frame.read_timeout = read_timeout;
  policy.frame.timer_executor = std::move(timer_executor);
  return policy;
}

std::vector<uint8_t> EncodeLengthPrefixedFrame(const std::vector<uint8_t>& body);
uint64_t DecodeLengthPrefixedHeader(const std::vector<uint8_t>& header8);

/** Reset a stream (error path / read timeout). Safe with null. */
void ResetStreamQuiet(const std::shared_ptr<libp2p::connection::Stream>& stream);

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
  /** Stops IO. Safe to call from inside FrameCallback (does not destroy on_frame_ reentrantly). */
  void Stop();

private:
  enum class Phase { Idle, Header, Body };

  void ReadHeader();
  void ReadBody(uint64_t payload_len);
  void ReleaseCallbackIfStopped();
  void ArmReadDeadline();
  void CancelReadDeadline();
  void OnReadDeadline();

  std::shared_ptr<libp2p::connection::Stream> stream_;
  FrameCallback on_frame_;
  StreamCancelCheck is_cancelled_;
  LengthPrefixedFrameConfig config_;
  std::atomic<bool> running_{false};
  Phase phase_ = Phase::Idle;
  libp2p::Bytes header_buf_;
  libp2p::Bytes payload_buf_;
  std::shared_ptr<boost::asio::steady_timer> read_timer_;
  std::shared_ptr<std::atomic<uint64_t>> deadline_generation_;
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

/**
 * Serialized length-prefixed duplex on one Yamux stream (host io thread).
 * Default: never overlaps read and write; write failure closes the session.
 * write_preferred=true (call-media / media-relay): full-duplex — drain outbound
 * during reads and keep reading while a write is in flight (avoids downlink
 * backpressure stalling uplink). A failed write drops the outbound queue and
 * keeps reading; only read/cancel/handler-close tears the session down (so a
 * stuck peer downlink cannot remove their uplink from the hop).
 */
class DuplexFrameSession : public std::enable_shared_from_this<DuplexFrameSession> {
public:
  /** Return false to close the session. */
  using FrameHandler = std::function<bool(Roe<std::vector<uint8_t>> body)>;
  /** reason is a stable short tag (e.g. read_eof, framing, handler, write_failed, stop). */
  using ClosedCallback = std::function<void(const char* reason)>;
  using WriteCallback = std::function<void(Roe<void>)>;

  void Start(std::shared_ptr<libp2p::connection::Stream> stream, FrameHandler on_frame,
             StreamCancelCheck is_cancelled, StreamIoPolicy policy = {},
             ClosedCallback on_closed = {});
  void Stop();

  /** Queue a frame body (length prefix added on write). Io-thread affine after Start. */
  bool EnqueueOutbound(std::vector<uint8_t> body, WriteCallback on_done = {});

  /** Queued frames + in-flight write (0 if idle). Safe from any thread. */
  size_t OutboundBacklog() const {
    return outbound_backlog_.load(std::memory_order_relaxed);
  }

private:
  struct PendingWrite {
    std::shared_ptr<std::vector<uint8_t>> frame;
    WriteCallback on_done;
  };

  void PublishBacklog();
  void BeginRead();
  void OnReadHeader(outcome::result<void> result);
  void OnReadBody(outcome::result<void> result);
  void DeliverFrame(std::vector<uint8_t> body);
  void PumpWrite();
  void MaybeResumeRead();
  void FailPendingWrites(const Error& error);
  void CloseSession(const char* reason);
  void ArmReadDeadline();
  void CancelReadDeadline();
  void OnReadDeadline();

  std::shared_ptr<libp2p::connection::Stream> stream_;
  FrameHandler on_frame_;
  StreamCancelCheck is_cancelled_;
  ClosedCallback on_closed_;
  LengthPrefixedFrameConfig config_;
  StreamIoPolicy::Drop drop_ = StreamIoPolicy::Drop::Never;
  size_t max_outbound_frames_ = 0;
  std::function<void()> on_outbound_drop_;
  bool write_preferred_ = false;
  bool read_once_ = false;
  bool read_completed_ = false;
  bool closed_ = false;
  std::atomic<bool> running_{false};
  std::atomic<size_t> outbound_backlog_{0};
  bool read_inflight_ = false;
  bool write_inflight_ = false;
  libp2p::Bytes header_buf_;
  libp2p::Bytes payload_buf_;
  std::vector<PendingWrite> outbound_;
  std::shared_ptr<boost::asio::steady_timer> read_timer_;
  std::shared_ptr<std::atomic<uint64_t>> deadline_generation_;
};

} // namespace pbr
