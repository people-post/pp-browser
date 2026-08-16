#include "base/p2p/StreamFrameIo.h"

#include "common/Logger.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <utility>

namespace pbr {

namespace {

using libp2p::Bytes;
using libp2p::connection::Stream;
using Clock = std::chrono::steady_clock;

bool IsCancelled(const StreamCancelCheck& check) {
  return check && check();
}

/** Wait for a libp2p::read completion; on deadline, reset the stream so the read fails. */
Roe<void> AwaitExactRead(std::future<outcome::result<void>>& future,
                         const std::shared_ptr<Stream>& stream,
                         const std::optional<Clock::time_point>& deadline,
                         const char* fail_message) {
  if (deadline) {
    if (future.wait_until(*deadline) != std::future_status::ready) {
      ResetStreamQuiet(stream);
      (void)future.get();
      return Error("length-prefixed frame read timed out");
    }
  }
  auto result = future.get();
  if (!result) {
    return Error(fail_message);
  }
  return {};
}

void CancelSteadyTimer(std::shared_ptr<boost::asio::steady_timer>& timer,
                       const std::shared_ptr<std::atomic<uint64_t>>& generation) {
  if (generation) {
    generation->fetch_add(1, std::memory_order_acq_rel);
  }
  if (timer) {
    (void)timer->cancel();
    timer.reset();
  }
}

} // namespace

void ResetStreamQuiet(const std::shared_ptr<Stream>& stream) {
  if (stream) {
    stream->reset();
  }
}

std::vector<uint8_t> EncodeLengthPrefixedFrame(const std::vector<uint8_t>& body) {
  std::vector<uint8_t> frame(8 + body.size());
  uint64_t len = body.size();
  for (int i = 7; i >= 0; --i) {
    frame[static_cast<size_t>(i)] = static_cast<uint8_t>(len & 0xff);
    len >>= 8;
  }
  if (!body.empty()) {
    std::memcpy(frame.data() + 8, body.data(), body.size());
  }
  return frame;
}

uint64_t DecodeLengthPrefixedHeader(const std::vector<uint8_t>& header8) {
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8 && i < header8.size(); ++i) {
    payload_len = (payload_len << 8) | header8[i];
  }
  return payload_len;
}

Roe<std::vector<uint8_t>> BlockingReadLengthPrefixedFrame(const std::shared_ptr<Stream>& stream,
                                                          const LengthPrefixedFrameConfig& config) {
  if (!stream) {
    return Error("Failed to read length-prefixed frame header");
  }

  std::optional<Clock::time_point> deadline;
  if (config.read_timeout.count() > 0) {
    deadline = Clock::now() + config.read_timeout;
  }

  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  if (auto wait = AwaitExactRead(header_future, stream, deadline, "Failed to read length-prefixed frame header");
      !wait) {
    return wait.error();
  }

  const uint64_t payload_len = DecodeLengthPrefixedHeader(std::vector<uint8_t>(header.begin(), header.end()));
  if (payload_len == 0 && !config.allow_empty_body) {
    ResetStreamQuiet(stream);
    return Error("length-prefixed frame empty");
  }
  if (payload_len > config.max_frame_bytes) {
    ResetStreamQuiet(stream);
    return Error("length-prefixed frame too large");
  }

  if (payload_len == 0) {
    return std::vector<uint8_t>{};
  }

  Bytes payload(static_cast<size_t>(payload_len));
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (auto wait = AwaitExactRead(body_future, stream, deadline, "Failed to read length-prefixed frame body");
      !wait) {
    return wait.error();
  }
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

Roe<void> BlockingWriteLengthPrefixedFrame(const std::shared_ptr<Stream>& stream,
                                           const std::vector<uint8_t>& body) {
  auto frame = EncodeLengthPrefixedFrame(body);
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  // Yamux WriteQueue stores BytesIn (span) — owned buffer must outlive the write callback.
  libp2p::write(stream, frame, [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write length-prefixed frame");
  }
  return {};
}

void AsyncLengthPrefixedReader::Start(std::shared_ptr<Stream> stream, FrameCallback on_frame,
                                      StreamCancelCheck is_cancelled, LengthPrefixedFrameConfig config) {
  if (!stream || !on_frame) {
    return;
  }
  stream_ = std::move(stream);
  on_frame_ = std::move(on_frame);
  is_cancelled_ = std::move(is_cancelled);
  config_ = std::move(config);
  deadline_generation_ = std::make_shared<std::atomic<uint64_t>>(0);
  running_.store(true, std::memory_order_release);
  ReadHeader();
}

void AsyncLengthPrefixedReader::Stop() {
  // Do not clear on_frame_ here: Stop() is often called from inside on_frame_
  // (one-shot AsyncReadStreamJson). Destroying std::function while it runs is UB.
  running_.store(false, std::memory_order_release);
  CancelReadDeadline();
  stream_.reset();
}

void AsyncLengthPrefixedReader::ReleaseCallbackIfStopped() {
  if (!running_.load(std::memory_order_acquire) || !stream_) {
    CancelReadDeadline();
    on_frame_ = {};
    is_cancelled_ = {};
  }
}

void AsyncLengthPrefixedReader::CancelReadDeadline() {
  CancelSteadyTimer(read_timer_, deadline_generation_);
}

void AsyncLengthPrefixedReader::ArmReadDeadline() {
  CancelReadDeadline();
  if (config_.read_timeout.count() <= 0 || !stream_) {
    return;
  }
  if (!config_.timer_executor) {
    logging::getLogger("StreamFrameIo").warning
        << "async frame read_timeout ignored (timer_executor unset)";
    return;
  }
  auto timer = std::make_shared<boost::asio::steady_timer>(config_.timer_executor);
  read_timer_ = timer;
  const uint64_t gen = deadline_generation_->load(std::memory_order_acquire);
  timer->expires_after(config_.read_timeout);
  auto self = shared_from_this();
  timer->async_wait([self, timer, gen](const boost::system::error_code& ec) {
    if (ec) {
      return;
    }
    if (!self->deadline_generation_ ||
        self->deadline_generation_->load(std::memory_order_acquire) != gen) {
      return;
    }
    self->OnReadDeadline();
  });
}

void AsyncLengthPrefixedReader::OnReadDeadline() {
  // Mark stopped and take the callback before reset — reset can synchronously complete the
  // in-flight read, whose !running_ path would otherwise clear on_frame_ first.
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  phase_ = Phase::Idle;
  CancelReadDeadline();
  FrameCallback cb;
  std::swap(cb, on_frame_);
  is_cancelled_ = {};
  ResetStreamQuiet(stream_);
  stream_.reset();
  if (cb) {
    cb(Error("length-prefixed frame read timed out"));
  }
}

void AsyncLengthPrefixedReader::ReadHeader() {
  if (!running_.load(std::memory_order_acquire) || IsCancelled(is_cancelled_) || !stream_) {
    ReleaseCallbackIfStopped();
    return;
  }
  header_buf_.assign(8, 0);
  phase_ = Phase::Header;
  ArmReadDeadline();
  auto self = shared_from_this();
  libp2p::read(stream_, header_buf_, [self](outcome::result<void> result) {
    if (!self->running_.load(std::memory_order_acquire) || IsCancelled(self->is_cancelled_)) {
      self->phase_ = Phase::Idle;
      self->CancelReadDeadline();
      // Not inside on_frame_ — safe to drop now (avoids reader↔callback cycle on cancel).
      self->on_frame_ = {};
      self->is_cancelled_ = {};
      return;
    }
    if (!result) {
      self->phase_ = Phase::Idle;
      self->CancelReadDeadline();
      if (self->on_frame_) {
        self->on_frame_(Error(std::string("Failed to read length-prefixed frame header: ") +
                              result.error().message()));
      }
      self->ReleaseCallbackIfStopped();
      return;
    }
    const uint64_t payload_len =
        DecodeLengthPrefixedHeader(std::vector<uint8_t>(self->header_buf_.begin(), self->header_buf_.end()));
    if (payload_len == 0 && !self->config_.allow_empty_body) {
      self->phase_ = Phase::Idle;
      self->CancelReadDeadline();
      ResetStreamQuiet(self->stream_);
      if (self->on_frame_) {
        self->on_frame_(Error("length-prefixed frame empty"));
      }
      self->ReleaseCallbackIfStopped();
      return;
    }
    if (payload_len > self->config_.max_frame_bytes) {
      self->phase_ = Phase::Idle;
      self->CancelReadDeadline();
      ResetStreamQuiet(self->stream_);
      if (self->on_frame_) {
        self->on_frame_(Error("length-prefixed frame too large"));
      }
      self->ReleaseCallbackIfStopped();
      return;
    }
    self->ReadBody(payload_len);
  });
}

void AsyncLengthPrefixedReader::ReadBody(uint64_t payload_len) {
  if (!running_.load(std::memory_order_acquire) || IsCancelled(is_cancelled_) || !stream_) {
    phase_ = Phase::Idle;
    CancelReadDeadline();
    ReleaseCallbackIfStopped();
    return;
  }
  if (payload_len == 0) {
    phase_ = Phase::Idle;
    CancelReadDeadline();
    if (on_frame_) {
      on_frame_(std::vector<uint8_t>{});
    }
    if (!running_.load(std::memory_order_acquire) || !stream_) {
      ReleaseCallbackIfStopped();
      return;
    }
    ReadHeader();
    return;
  }

  payload_buf_.resize(static_cast<size_t>(payload_len));
  phase_ = Phase::Body;
  auto self = shared_from_this();
  libp2p::read(stream_, payload_buf_, [self](outcome::result<void> result) {
    self->phase_ = Phase::Idle;
    self->CancelReadDeadline();
    if (!self->running_.load(std::memory_order_acquire) || IsCancelled(self->is_cancelled_)) {
      self->on_frame_ = {};
      self->is_cancelled_ = {};
      return;
    }
    if (!result) {
      if (self->on_frame_) {
        self->on_frame_(Error(std::string("Failed to read length-prefixed frame body: ") +
                              result.error().message()));
      }
      self->ReleaseCallbackIfStopped();
      return;
    }
    std::vector<uint8_t> body(self->payload_buf_.begin(), self->payload_buf_.end());
    if (self->on_frame_) {
      self->on_frame_(std::move(body));
    }
    // One-shot readers Stop() inside on_frame_; drop callback only after it returns.
    if (!self->running_.load(std::memory_order_acquire) || !self->stream_) {
      self->ReleaseCallbackIfStopped();
      return;
    }
    self->ReadHeader();
  });
}

void StreamBridge::Start(std::shared_ptr<Stream> from, std::shared_ptr<Stream> to, StreamCancelCheck is_cancelled,
                         std::function<void()> on_closed, size_t chunk_bytes) {
  from_ = std::move(from);
  to_ = std::move(to);
  is_cancelled_ = std::move(is_cancelled);
  on_closed_ = std::move(on_closed);
  chunk_bytes_ = chunk_bytes == 0 ? kDefaultChunkBytes : chunk_bytes;
  chunk_buf_.assign(chunk_bytes_, 0);
  running_.store(true, std::memory_order_release);
  PumpRead();
}

void StreamBridge::Stop() {
  running_.store(false, std::memory_order_release);
  from_.reset();
  to_.reset();
  on_closed_ = {};
  is_cancelled_ = {};
}

void StreamBridge::PumpRead() {
  if (!running_.load(std::memory_order_acquire) || read_inflight_ || !from_ || !to_) {
    return;
  }
  if (IsCancelled(is_cancelled_)) {
    Stop();
    if (on_closed_) {
      on_closed_();
    }
    return;
  }

  read_inflight_ = true;
  auto self = shared_from_this();
  from_->readSome(chunk_buf_, [self](outcome::result<size_t> read_res) {
    self->read_inflight_ = false;
    if (!self->running_.load(std::memory_order_acquire) || IsCancelled(self->is_cancelled_)) {
      self->Stop();
      if (self->on_closed_) {
        self->on_closed_();
      }
      return;
    }
    if (!read_res) {
      self->Stop();
      if (self->on_closed_) {
        self->on_closed_();
      }
      return;
    }
    const size_t n = read_res.value();
    if (n == 0) {
      self->Stop();
      if (self->on_closed_) {
        self->on_closed_();
      }
      return;
    }

    auto write_buf = std::make_shared<libp2p::Bytes>(self->chunk_buf_.begin(),
                                                     self->chunk_buf_.begin() + static_cast<ptrdiff_t>(n));
    libp2p::write(self->to_, *write_buf, [self, write_buf](outcome::result<void> write_res) {
      if (!self->running_.load(std::memory_order_acquire) || IsCancelled(self->is_cancelled_)) {
        self->Stop();
        if (self->on_closed_) {
          self->on_closed_();
        }
        return;
      }
      if (!write_res) {
        self->Stop();
        if (self->on_closed_) {
          self->on_closed_();
        }
        return;
      }
      self->PumpRead();
    });
  });
}

void DuplexFrameSession::CancelReadDeadline() {
  CancelSteadyTimer(read_timer_, deadline_generation_);
}

void DuplexFrameSession::ArmReadDeadline() {
  CancelReadDeadline();
  if (config_.read_timeout.count() <= 0 || !stream_) {
    return;
  }
  if (!config_.timer_executor) {
    return;
  }
  auto timer = std::make_shared<boost::asio::steady_timer>(config_.timer_executor);
  read_timer_ = timer;
  const uint64_t gen = deadline_generation_->load(std::memory_order_acquire);
  timer->expires_after(config_.read_timeout);
  auto self = shared_from_this();
  timer->async_wait([self, timer, gen](const boost::system::error_code& ec) {
    if (ec) {
      return;
    }
    if (!self->deadline_generation_ ||
        self->deadline_generation_->load(std::memory_order_acquire) != gen) {
      return;
    }
    self->OnReadDeadline();
  });
}

void DuplexFrameSession::OnReadDeadline() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  // Take the handler before reset — reset may synchronously finish the in-flight read.
  FrameHandler handler;
  std::swap(handler, on_frame_);
  ResetStreamQuiet(stream_);
  if (handler) {
    handler(Error("length-prefixed frame read timed out"));
  }
  CloseSession("read_timeout");
}

void DuplexFrameSession::Start(std::shared_ptr<Stream> stream, FrameHandler on_frame,
                               StreamCancelCheck is_cancelled, StreamIoPolicy policy,
                               ClosedCallback on_closed) {
  if (!stream || !on_frame) {
    return;
  }
  stream_ = std::move(stream);
  on_frame_ = std::move(on_frame);
  is_cancelled_ = std::move(is_cancelled);
  on_closed_ = std::move(on_closed);
  config_ = std::move(policy.frame);
  drop_ = policy.drop;
  max_outbound_frames_ = policy.max_outbound_frames;
  on_outbound_drop_ = std::move(policy.on_outbound_drop);
  write_preferred_ = policy.write_preferred;
  read_once_ = policy.read_once;
  read_completed_ = false;
  closed_ = false;
  deadline_generation_ = std::make_shared<std::atomic<uint64_t>>(0);
  running_.store(true, std::memory_order_release);
  PumpWrite();
  MaybeResumeRead();
}

void DuplexFrameSession::Stop() {
  closed_ = true;
  running_.store(false, std::memory_order_release);
  CancelReadDeadline();
  read_inflight_ = false;
  write_inflight_ = false;
  FailPendingWrites(Error("duplex stopped"));
  outbound_backlog_.store(0, std::memory_order_relaxed);
  stream_.reset();
  on_frame_ = {};
  is_cancelled_ = {};
  on_closed_ = {};
}

void DuplexFrameSession::PublishBacklog() {
  const size_t n = outbound_.size() + (write_inflight_ ? 1u : 0u);
  outbound_backlog_.store(n, std::memory_order_relaxed);
}

void DuplexFrameSession::FailPendingWrites(const Error& error) {
  std::vector<PendingWrite> pending;
  pending.swap(outbound_);
  for (auto& item : pending) {
    if (item.on_done) {
      item.on_done(error);
    }
  }
}

bool DuplexFrameSession::EnqueueOutbound(std::vector<uint8_t> body, WriteCallback on_done) {
  if (closed_ || (running_.load(std::memory_order_acquire) && !stream_)) {
    return false;
  }
  if (max_outbound_frames_ > 0 && outbound_.size() >= max_outbound_frames_) {
    if (drop_ != StreamIoPolicy::Drop::Oldest) {
      return false;
    }
    PendingWrite dropped = std::move(outbound_.front());
    outbound_.erase(outbound_.begin());
    if (dropped.on_done) {
      dropped.on_done(Error("outbound frame dropped"));
    }
    if (on_outbound_drop_) {
      on_outbound_drop_();
    }
  }
  PendingWrite pending;
  pending.frame = std::make_shared<std::vector<uint8_t>>(EncodeLengthPrefixedFrame(body));
  pending.on_done = std::move(on_done);
  outbound_.push_back(std::move(pending));
  PublishBacklog();
  if (running_.load(std::memory_order_acquire) &&
      (write_preferred_ || (!read_inflight_ && !write_inflight_))) {
    PumpWrite();
  }
  return true;
}

void DuplexFrameSession::BeginRead() {
  if (!running_.load(std::memory_order_acquire) || IsCancelled(is_cancelled_) || !stream_) {
    CloseSession(IsCancelled(is_cancelled_) ? "cancelled" : "begin_read_idle");
    return;
  }
  if (read_inflight_) {
    return;
  }
  // Half-duplex mode: wait for writes to finish. write_preferred = full duplex so a
  // stuck/slow peer write cannot stop reading their uplink (hop fan-in).
  if (!write_preferred_ && (write_inflight_ || !outbound_.empty())) {
    return;
  }
  read_inflight_ = true;
  header_buf_.assign(8, 0);
  ArmReadDeadline();
  auto self = shared_from_this();
  libp2p::read(stream_, header_buf_, [self](outcome::result<void> result) { self->OnReadHeader(result); });
}

void DuplexFrameSession::OnReadHeader(outcome::result<void> result) {
  if (!running_.load(std::memory_order_acquire) || IsCancelled(is_cancelled_)) {
    read_inflight_ = false;
    CancelReadDeadline();
    CloseSession("cancelled");
    return;
  }
  if (!result) {
    read_inflight_ = false;
    CancelReadDeadline();
    CloseSession("read_eof");
    return;
  }
  const uint64_t payload_len =
      DecodeLengthPrefixedHeader(std::vector<uint8_t>(header_buf_.begin(), header_buf_.end()));
  if (payload_len == 0 && !config_.allow_empty_body) {
    read_inflight_ = false;
    CancelReadDeadline();
    ResetStreamQuiet(stream_);
    if (on_frame_) {
      on_frame_(Error("length-prefixed frame empty"));
    }
    CloseSession("empty_frame");
    return;
  }
  if (payload_len > config_.max_frame_bytes) {
    read_inflight_ = false;
    CancelReadDeadline();
    ResetStreamQuiet(stream_);
    if (on_frame_) {
      on_frame_(Error("length-prefixed frame too large"));
    }
    CloseSession("frame_too_large");
    return;
  }
  if (payload_len == 0) {
    CancelReadDeadline();
    DeliverFrame({});
    return;
  }
  if (write_preferred_) {
    PumpWrite();
  }
  payload_buf_.resize(static_cast<size_t>(payload_len));
  auto self = shared_from_this();
  libp2p::read(stream_, payload_buf_, [self](outcome::result<void> body_result) { self->OnReadBody(body_result); });
}

void DuplexFrameSession::OnReadBody(outcome::result<void> result) {
  CancelReadDeadline();
  if (!running_.load(std::memory_order_acquire) || IsCancelled(is_cancelled_)) {
    read_inflight_ = false;
    CloseSession("cancelled");
    return;
  }
  if (!result) {
    read_inflight_ = false;
    CloseSession("read_eof");
    return;
  }
  DeliverFrame(std::vector<uint8_t>(payload_buf_.begin(), payload_buf_.end()));
}

void DuplexFrameSession::DeliverFrame(std::vector<uint8_t> body) {
  read_inflight_ = false;
  if (read_once_) {
    read_completed_ = true;
  }
  bool keep_open = true;
  if (on_frame_) {
    keep_open = on_frame_(std::move(body));
  }
  if (!keep_open || !running_.load(std::memory_order_acquire)) {
    CloseSession(keep_open ? "stopped" : "handler_close");
    return;
  }
  PumpWrite();
  MaybeResumeRead();
}

void DuplexFrameSession::PumpWrite() {
  if (!running_.load(std::memory_order_acquire) || write_inflight_ || !stream_) {
    PublishBacklog();
    MaybeResumeRead();
    return;
  }
  if (outbound_.empty()) {
    PublishBacklog();
    MaybeResumeRead();
    return;
  }
  PendingWrite pending = std::move(outbound_.front());
  outbound_.erase(outbound_.begin());
  write_inflight_ = true;
  PublishBacklog();
  auto self = shared_from_this();
  libp2p::write(stream_, *pending.frame,
                [self, pending = std::move(pending)](outcome::result<void> result) mutable {
    self->write_inflight_ = false;
    self->PublishBacklog();
    const bool running = self->running_.load(std::memory_order_acquire);
    if (pending.on_done) {
      if (!running) {
        pending.on_done(Error("duplex stopped"));
      } else if (!result) {
        pending.on_done(Error(std::string("Failed to write length-prefixed frame: ") +
                              result.error().message()));
      } else {
        pending.on_done({});
      }
    }
    if (!running) {
      return;
    }
    if (!result) {
      if (self->write_preferred_) {
        // Downlink write failed — drop queued frames and keep reading uplink.
        // Closing here removed hop participants mid-call while phones still TX'd.
        static std::atomic<int> write_keep_log{0};
        const int n = write_keep_log.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n % 50) == 0) {
          logging::getLogger("DuplexFrameSession").warning
              << "write_failed_kept_open n=" << n;
        }
        self->FailPendingWrites(Error("outbound write failed"));
        self->PublishBacklog();
        if (self->on_outbound_drop_) {
          self->on_outbound_drop_();
        }
        self->MaybeResumeRead();
        return;
      }
      self->CloseSession("write_failed");
      return;
    }
    self->PumpWrite();
  });
}

void DuplexFrameSession::MaybeResumeRead() {
  if (!running_.load(std::memory_order_acquire) || read_inflight_ || IsCancelled(is_cancelled_) ||
      !stream_) {
    return;
  }
  if (read_once_ && read_completed_) {
    return;
  }
  if (!write_preferred_ && (write_inflight_ || !outbound_.empty())) {
    return;
  }
  BeginRead();
}

void DuplexFrameSession::CloseSession(const char* reason) {
  // Keep alive across on_closed_ — CleanupParticipant may reset the last owning shared_ptr.
  std::shared_ptr<DuplexFrameSession> keep;
  try {
    keep = shared_from_this();
  } catch (const std::bad_weak_ptr&) {
  }
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  closed_ = true;
  const char* tag = (reason && reason[0]) ? reason : "unknown";
  logging::getLogger("DuplexFrameSession").warning << "CloseSession reason=" << tag;
  CancelReadDeadline();
  read_inflight_ = false;
  write_inflight_ = false;
  FailPendingWrites(Error(std::string("duplex closed: ") + tag));
  outbound_backlog_.store(0, std::memory_order_relaxed);
  stream_.reset();
  on_frame_ = {};
  is_cancelled_ = {};
  ClosedCallback cb;
  std::swap(cb, on_closed_);
  if (cb) {
    cb(tag);
  }
}

} // namespace pbr
