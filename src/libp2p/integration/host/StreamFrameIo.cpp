#include "libp2p/integration/host/StreamFrameIo.h"

#include "common/Logger.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>

#include <cstring>
#include <future>
#include <atomic>
#include <utility>

namespace pbr {

namespace {

using libp2p::Bytes;
using libp2p::connection::Stream;

bool IsCancelled(const StreamCancelCheck& check) {
  return check && check();
}

} // namespace

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
  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  if (!header_future.get()) {
    return Error("Failed to read length-prefixed frame header");
  }

  const uint64_t payload_len = DecodeLengthPrefixedHeader(std::vector<uint8_t>(header.begin(), header.end()));
  if (payload_len == 0 && !config.allow_empty_body) {
    return Error("length-prefixed frame empty");
  }
  if (payload_len > config.max_frame_bytes) {
    return Error("length-prefixed frame too large");
  }

  if (payload_len == 0) {
    return std::vector<uint8_t>{};
  }

  Bytes payload(static_cast<size_t>(payload_len));
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read length-prefixed frame body");
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
  config_ = config;
  running_.store(true, std::memory_order_release);
  ReadHeader();
}

void AsyncLengthPrefixedReader::Stop() {
  running_.store(false, std::memory_order_release);
  stream_.reset();
  on_frame_ = {};
  is_cancelled_ = {};
}

void AsyncLengthPrefixedReader::ReadHeader() {
  if (!running_.load(std::memory_order_acquire) || IsCancelled(is_cancelled_) || !stream_) {
    return;
  }
  header_buf_.assign(8, 0);
  phase_ = Phase::Header;
  auto self = shared_from_this();
  libp2p::read(stream_, header_buf_, [self](outcome::result<void> result) {
    if (!self->running_.load(std::memory_order_acquire) || IsCancelled(self->is_cancelled_)) {
      self->phase_ = Phase::Idle;
      return;
    }
    if (!result) {
      self->phase_ = Phase::Idle;
      if (self->on_frame_) {
        self->on_frame_(Error(std::string("Failed to read length-prefixed frame header: ") +
                              result.error().message()));
      }
      return;
    }
    const uint64_t payload_len =
        DecodeLengthPrefixedHeader(std::vector<uint8_t>(self->header_buf_.begin(), self->header_buf_.end()));
    if (payload_len == 0 && !self->config_.allow_empty_body) {
      self->phase_ = Phase::Idle;
      if (self->on_frame_) {
        self->on_frame_(Error("length-prefixed frame empty"));
      }
      return;
    }
    if (payload_len > self->config_.max_frame_bytes) {
      self->phase_ = Phase::Idle;
      if (self->on_frame_) {
        self->on_frame_(Error("length-prefixed frame too large"));
      }
      return;
    }
    self->ReadBody(payload_len);
  });
}

void AsyncLengthPrefixedReader::ReadBody(uint64_t payload_len) {
  if (!running_.load(std::memory_order_acquire) || IsCancelled(is_cancelled_) || !stream_) {
    phase_ = Phase::Idle;
    return;
  }
  if (payload_len == 0) {
    phase_ = Phase::Idle;
    if (on_frame_) {
      on_frame_(std::vector<uint8_t>{});
      ReadHeader();
    }
    return;
  }

  payload_buf_.resize(static_cast<size_t>(payload_len));
  phase_ = Phase::Body;
  auto self = shared_from_this();
  libp2p::read(stream_, payload_buf_, [self](outcome::result<void> result) {
    self->phase_ = Phase::Idle;
    if (!self->running_.load(std::memory_order_acquire) || IsCancelled(self->is_cancelled_)) {
      return;
    }
    if (!result) {
      if (self->on_frame_) {
        self->on_frame_(Error(std::string("Failed to read length-prefixed frame body: ") +
                              result.error().message()));
      }
      return;
    }
    std::vector<uint8_t> body(self->payload_buf_.begin(), self->payload_buf_.end());
    if (self->on_frame_) {
      self->on_frame_(std::move(body));
    }
    self->ReadHeader();
  });
}

void AsyncLengthPrefixedWriter::Start(std::shared_ptr<Stream> stream) {
  stream_ = std::move(stream);
  running_.store(true, std::memory_order_release);
  PumpWrite();
}

void AsyncLengthPrefixedWriter::Stop() {
  running_.store(false, std::memory_order_release);
  queue_.clear();
  stream_.reset();
}

bool AsyncLengthPrefixedWriter::Enqueue(const std::vector<uint8_t>& body, WriteCallback on_done) {
  if (!running_.load(std::memory_order_acquire) || !stream_) {
    return false;
  }
  PendingWrite pending;
  pending.frame = std::make_shared<std::vector<uint8_t>>(EncodeLengthPrefixedFrame(body));
  pending.on_done = std::move(on_done);
  queue_.push_back(std::move(pending));
  PumpWrite();
  return true;
}

void AsyncLengthPrefixedWriter::PumpWrite() {
  if (!running_.load(std::memory_order_acquire) || write_inflight_ || !stream_) {
    return;
  }
  if (queue_.empty()) {
    return;
  }
  PendingWrite pending = std::move(queue_.front());
  queue_.erase(queue_.begin());
  write_inflight_ = true;
  auto self = shared_from_this();
  libp2p::write(stream_, *pending.frame, [self, pending = std::move(pending)](outcome::result<void> result) mutable {
    self->write_inflight_ = false;
    if (pending.on_done) {
      if (result) {
        pending.on_done({});
      } else {
        pending.on_done(Error(std::string("Failed to write length-prefixed frame: ") + result.error().message()));
      }
    }
    if (!self->running_.load(std::memory_order_acquire)) {
      return;
    }
    if (!result) {
      self->Stop();
      return;
    }
    self->PumpWrite();
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

void DuplexFrameSession::Start(std::shared_ptr<Stream> stream, FrameHandler on_frame,
                               StreamCancelCheck is_cancelled, LengthPrefixedFrameConfig config,
                               ClosedCallback on_closed, size_t max_outbound_frames,
                               std::function<void()> on_outbound_drop, bool write_preferred) {
  if (!stream || !on_frame) {
    return;
  }
  stream_ = std::move(stream);
  on_frame_ = std::move(on_frame);
  is_cancelled_ = std::move(is_cancelled);
  on_closed_ = std::move(on_closed);
  config_ = config;
  max_outbound_frames_ = max_outbound_frames;
  on_outbound_drop_ = std::move(on_outbound_drop);
  write_preferred_ = write_preferred;
  running_.store(true, std::memory_order_release);
  PumpWrite();
  MaybeResumeRead();
}

void DuplexFrameSession::Stop() {
  running_.store(false, std::memory_order_release);
  read_inflight_ = false;
  write_inflight_ = false;
  outbound_.clear();
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

bool DuplexFrameSession::EnqueueOutbound(std::vector<uint8_t> body) {
  if (!running_.load(std::memory_order_acquire) || !stream_) {
    return false;
  }
  if (max_outbound_frames_ > 0 && outbound_.size() >= max_outbound_frames_) {
    outbound_.erase(outbound_.begin());
    if (on_outbound_drop_) {
      on_outbound_drop_();
    }
  }
  outbound_.push_back(std::make_shared<std::vector<uint8_t>>(EncodeLengthPrefixedFrame(body)));
  PublishBacklog();
  if (write_preferred_ || (!read_inflight_ && !write_inflight_)) {
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
  auto self = shared_from_this();
  libp2p::read(stream_, header_buf_, [self](outcome::result<void> result) { self->OnReadHeader(result); });
}

void DuplexFrameSession::OnReadHeader(outcome::result<void> result) {
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
  const uint64_t payload_len =
      DecodeLengthPrefixedHeader(std::vector<uint8_t>(header_buf_.begin(), header_buf_.end()));
  if (payload_len == 0 && !config_.allow_empty_body) {
    read_inflight_ = false;
    if (on_frame_) {
      on_frame_(Error("length-prefixed frame empty"));
    }
    CloseSession("empty_frame");
    return;
  }
  if (payload_len > config_.max_frame_bytes) {
    read_inflight_ = false;
    if (on_frame_) {
      on_frame_(Error("length-prefixed frame too large"));
    }
    CloseSession("frame_too_large");
    return;
  }
  if (payload_len == 0) {
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
  auto frame = outbound_.front();
  outbound_.erase(outbound_.begin());
  write_inflight_ = true;
  PublishBacklog();
  auto self = shared_from_this();
  libp2p::write(stream_, *frame, [self, frame](outcome::result<void> result) {
    self->write_inflight_ = false;
    self->PublishBacklog();
    if (!self->running_.load(std::memory_order_acquire)) {
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
        self->outbound_.clear();
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
  const char* tag = (reason && reason[0]) ? reason : "unknown";
  logging::getLogger("DuplexFrameSession").warning << "CloseSession reason=" << tag;
  read_inflight_ = false;
  write_inflight_ = false;
  outbound_.clear();
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
