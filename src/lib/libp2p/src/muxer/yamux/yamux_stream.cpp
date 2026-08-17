/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/muxer/yamux/yamux_stream.hpp>

#include <cassert>
#include <mutex>
#include <optional>
#include <vector>

#include <libp2p/muxer/yamux/yamux_frame.hpp>
#include <qtils/option_take.hpp>

#define TRACE_ENABLED 0
#include <libp2p/common/trace.hpp>

namespace libp2p::connection {

  namespace {
    auto log() {
      static auto logger = log::createLogger("yx-stream");
      return logger.get();
    }
  }  // namespace

  /**
   * Calls read callback on return.
   *
   * try { ... } finally { cb(); }
   */
  struct FinallyReading {
    FinallyReading(YamuxStream::ReadCallbackFunc cb, outcome::result<size_t> r)
        : cb{std::move(cb)}, r{r} {}
    ~FinallyReading() {
      cb(r);
    }

    // clang-tidy cppcoreguidelines-special-member-functions
    FinallyReading(const FinallyReading &) = delete;
    void operator=(const FinallyReading &) = delete;
    FinallyReading(FinallyReading &&) = delete;
    void operator=(FinallyReading &&) = delete;

    YamuxStream::ReadCallbackFunc cb;
    outcome::result<size_t> r;
  };

  YamuxStream::YamuxStream(
      std::shared_ptr<connection::SecureConnection> connection,
      YamuxStreamFeedback &feedback,
      uint32_t stream_id,
      size_t maximum_window_size,
      size_t write_queue_limit)
      : connection_(std::move(connection)),
        feedback_(feedback),
        stream_id_(stream_id),
        window_size_(YamuxFrame::kInitialWindowSize),
        peers_window_size_(YamuxFrame::kInitialWindowSize),
        maximum_window_size_(maximum_window_size),
        write_queue_(write_queue_limit) {
    assert(connection_);
    assert(stream_id_ > 0);
    assert(window_size_ <= maximum_window_size_);
    assert(peers_window_size_ <= maximum_window_size_);
    assert(write_queue_limit >= maximum_window_size_);
  }

  void YamuxStream::readSome(BytesOut out, ReadCallbackFunc cb) {
    doRead(out, std::move(cb));
  }

  void YamuxStream::deferReadCallback(outcome::result<size_t> res,
                                      ReadCallbackFunc cb) {
    feedback_.deferCall([res, cb{std::move(cb)}] { cb(res); });
  }

  void YamuxStream::writeSome(BytesIn in, WriteCallbackFunc cb) {
    doWrite(in, std::move(cb));
  }

  void YamuxStream::deferWriteCallback(std::error_code ec,
                                       WriteCallbackFunc cb) {
    feedback_.deferCall([ec, cb{std::move(cb)}] { cb(ec); });
  }

  bool YamuxStream::isClosed() const {
    return close_reason_.has_value();
  }

  void YamuxStream::close(VoidResultHandlerFunc cb) {
    if (isClosed()) {
      if (cb) {
        feedback_.deferCall(
            [cb{std::move(cb)}, ec{*close_reason_}] { cb(ec); });
      }
      return;
    }

    close_cb_ = std::move(cb);

    if (!isClosedForWrite()) {
      bool finish_close = false;
      {
        std::unique_lock lock(stream_write_mu_);
        is_writable_ = false;
        finish_close = doWriteUnlocked();
      }
      if (finish_close) {
        doClose(Error::STREAM_CLOSED_BY_HOST);
      }
    }
  }

  std::pair<YamuxStream::VoidResultHandlerFunc, outcome::result<void>>
  YamuxStream::closeCompleted() {
    std::pair<VoidResultHandlerFunc, outcome::result<void>> p{
        VoidResultHandlerFunc{}, outcome::success()};
    if (!close_reason_) {
      close_reason_ = Error::STREAM_CLOSED_BY_HOST;
    } else if (close_reason_ != Error::STREAM_CLOSED_BY_HOST) {
      p.second = *close_reason_;
    }
    if (close_cb_) {
      p.first.swap(close_cb_);
    }
    return p;
  }

  bool YamuxStream::isClosedForRead() const {
    return !is_readable_;
  }

  bool YamuxStream::isClosedForWrite() const {
    return !is_writable_;
  }

  void YamuxStream::reset() {
    feedback_.resetStream(stream_id_);
    doClose(Error::STREAM_RESET_BY_HOST);
  }

  void YamuxStream::adjustWindowSize(uint32_t new_size,
                                     VoidResultHandlerFunc cb) {
    auto ec = close_reason_;
    if (!ec) {
      if (!is_readable_) {
        ec = Error::STREAM_NOT_READABLE;
      } else if (new_size > maximum_window_size_
                 || new_size < peers_window_size_) {
        ec = Error::STREAM_INVALID_WINDOW_SIZE;
      }
    }

    if (!ec && new_size > peers_window_size_) {
      // Doing this optimistic way, if other side don't like the window update
      // then it would RST

      feedback_.ackReceivedBytes(stream_id_, new_size - peers_window_size_);
      peers_window_size_ = new_size;
    }

    if (cb) {
      feedback_.deferCall([cb{std::move(cb)}, ec] {
        if (!ec) {
          cb(outcome::success());
        } else {
          cb(*ec);
        }
      });
    }
  }

  outcome::result<peer::PeerId> YamuxStream::remotePeerId() const {
    return connection_->remotePeer();
  }

  outcome::result<bool> YamuxStream::isInitiator() const {
    return connection_->isInitiator();
  }

  outcome::result<multi::Multiaddress> YamuxStream::localMultiaddr() const {
    return connection_->localMultiaddr();
  }

  outcome::result<multi::Multiaddress> YamuxStream::remoteMultiaddr() const {
    return connection_->remoteMultiaddr();
  }

  void YamuxStream::increaseSendWindow(size_t delta) {
    bool finish_close = false;
    {
      std::unique_lock lock(stream_write_mu_);
      if (delta > 0) {
        window_size_ += delta;
        TRACE("stream {} send window increased by {} to {}",
              stream_id_,
              delta,
              window_size_);
        finish_close = doWriteUnlocked();
      }
    }
    if (finish_close) {
      doClose(Error::STREAM_CLOSED_BY_HOST);
    }
  }

  YamuxStream::DataFromConnectionResult YamuxStream::onDataReceived(
      BytesOut bytes) {
    auto sz = static_cast<size_t>(bytes.size());

    if (sz == 0) {
      log()->critical("zero data packet received - should not get here");
      return kKeepStream;
    }

    TRACE("stream {} read {} bytes", stream_id_, sz);

    bool overflow = false;
    size_t bytes_consumed = 0;
    std::optional<FinallyReading> finally_reading;

    // First transfer bytes to client if available
    if (auto reading = qtils::optionTake(reading_)) {
      // Yamux invariant: pending read ⇒ buffer empty. Prior soft-heals can leave
      // leftovers (media_relay dogfood: assert empty aborted moto pp-worker).
      if (!internal_read_buffer_.empty()) {
        log()->warn("yamux stream {} onDataReceived: draining non-empty buffer "
                    "before pending read (size={})",
                    stream_id_,
                    internal_read_buffer_.size());
        size_t drained = internal_read_buffer_.consume(reading->out);
        if (drained >= static_cast<size_t>(reading->out.size())) {
          finally_reading.emplace(std::move(reading->cb), drained);
          internal_read_buffer_.add(bytes);
          bytes_consumed = drained;
        } else if (drained > 0) {
          auto rest = reading->out.subspan(drained);
          size_t more = internal_read_buffer_.addAndConsume(bytes, rest);
          bytes_consumed = drained + more;
          if (bytes_consumed == 0) {
            internal_read_buffer_.clear();
            internal_read_buffer_.add(bytes);
            reading_.emplace(Reading{reading->out, std::move(reading->cb)});
            return kKeepStream;
          }
          finally_reading.emplace(std::move(reading->cb), bytes_consumed);
        } else {
          internal_read_buffer_.clear();
          bytes_consumed =
              internal_read_buffer_.addAndConsume(bytes, reading->out);
          if (bytes_consumed == 0) {
            internal_read_buffer_.add(bytes);
            reading_.emplace(Reading{reading->out, std::move(reading->cb)});
            return kKeepStream;
          }
          finally_reading.emplace(std::move(reading->cb), bytes_consumed);
        }
      } else {
        bytes_consumed = internal_read_buffer_.addAndConsume(bytes, reading->out);
        if (bytes_consumed == 0) {
          // Soft-fail: keep pending read and buffer inbound (do not assert).
          internal_read_buffer_.add(bytes);
          reading_.emplace(Reading{reading->out, std::move(reading->cb)});
          return kKeepStream;
        }
        finally_reading.emplace(std::move(reading->cb), bytes_consumed);
      }
    } else {
      internal_read_buffer_.add(bytes);
    }

    if (!internal_read_buffer_.empty()) {
      overflow = (internal_read_buffer_.size() > peers_window_size_);
      if (overflow) {
        log()->debug("read buffer overflow {} > {}, stream {}",
                     internal_read_buffer_.size(),
                     peers_window_size_,
                     stream_id_);
      } else {
        TRACE("stream {} receive window reduced by {} to {}",
              stream_id_,
              internal_read_buffer_.size(),
              peers_window_size_ - internal_read_buffer_.size());
      }
    }

    if (isClosed()) {
      // already closed, maybe error
      return kRemoveStreamAndSendRst;
    }

    if (overflow) {
      doClose(Error::STREAM_RECEIVE_OVERFLOW);
    } else if (bytes_consumed > 0) {
      feedback_.ackReceivedBytes(stream_id_, bytes_consumed);
      TRACE("stream {} receive window increased by {} to {}",
            stream_id_,
            bytes_consumed,
            peers_window_size_ - internal_read_buffer_.size());
    }

    return overflow ? kRemoveStreamAndSendRst : kKeepStream;
  }

  YamuxStream::DataFromConnectionResult YamuxStream::onFINReceived() {
    std::optional<FinallyReading> finally_reading;
    if (auto reading = qtils::optionTake(reading_)) {
      finally_reading.emplace(std::move(reading->cb),
                              Error::STREAM_CLOSED_BY_HOST);
    }

    if (isClosed()) {
      // already closed, maybe error
      return kRemoveStreamAndSendRst;
    }

    is_readable_ = false;

    if (!is_writable_) {
      doClose(Error::STREAM_CLOSED_BY_HOST);

      // connection will remove stream
      return kRemoveStream;
    }

    return kKeepStream;
  }

  void YamuxStream::onRSTReceived() {
    if (isClosed()) {
      // already closed, maybe error
      return;
    }

    doClose(Error::STREAM_RESET_BY_PEER);
  }

  void YamuxStream::onDataWritten(size_t bytes) {
    std::unique_lock lock(stream_write_mu_);
    auto result = write_queue_.ackDataSent(bytes);
    if (!result.data_consistent) {
      log()->error("write queue ack failed, stream {}", stream_id_);
      lock.unlock();
      feedback_.resetStream(stream_id_);
      doClose(Error::STREAM_INTERNAL_ERROR);
      return;
    }

    // Unlock before user callback — may re-enter writeSome.
    auto cb = std::move(result.cb);
    const size_t size_to_ack = result.size_to_ack;
    lock.unlock();
    if (cb) {
      cb(size_to_ack);
    }
  }

  void YamuxStream::closedByConnection(std::error_code ec) {
    doClose(std::move(ec));
  }

  void YamuxStream::doClose(std::error_code ec) {
    // ensure lifetime of this object during doClose
    auto self = shared_from_this();

    std::optional<FinallyReading> finally_reading;
    if (auto reading = qtils::optionTake(reading_)) {
      finally_reading.emplace(std::move(reading->cb), ec);
    }

    std::vector<basic::Writer::WriteCallbackFunc> write_callbacks;
    VoidResultHandlerFunc window_size_cb;
    VoidResultHandlerFunc close_cb;
    outcome::result<void> close_res = outcome::success();
    {
      std::lock_guard lock(stream_write_mu_);
      if (close_reason_) {
        // already closed
        return;
      }

      close_reason_ = ec;
      is_readable_ = false;
      is_writable_ = false;

      internal_read_buffer_.clear();

      write_callbacks = write_queue_.getAllCallbacks();
      write_queue_.clear();

      auto close_cb_and_res = closeCompleted();
      close_cb = std::move(close_cb_and_res.first);
      close_res = close_cb_and_res.second;
      window_size_cb.swap(window_size_cb_);
    }

    for (const auto &cb : write_callbacks) {
      cb(ec);
    }

    if (window_size_cb) {
      window_size_cb(ec);
    }

    if (close_cb) {
      close_cb(close_res);
    }
  }

  void YamuxStream::doRead(BytesOut out, ReadCallbackFunc cb) {
    assert(cb);

    if (out.empty()) {
      return deferReadCallback(Error::STREAM_INVALID_ARGUMENT, std::move(cb));
    }

    // If something is still in read buffer, the client can consume these bytes
    auto bytes_available_now = internal_read_buffer_.size();
    if (bytes_available_now > 0) {
      size_t consumed = internal_read_buffer_.consume(out);

      // Soft-fail: ReadBuffer can report size>0 then yield 0 after healing an
      // inconsistent fragment (media_relay dogfood: moto/Samsung assert abort).
      // Buffer MUST be empty before arming reading_ (onDataReceived invariant).
      if (consumed == 0) {
        internal_read_buffer_.clear();
        if (close_reason_) {
          return deferReadCallback(*close_reason_, std::move(cb));
        }
        if (!is_readable_) {
          return deferReadCallback(Error::STREAM_NOT_READABLE, std::move(cb));
        }
        if (reading_.has_value()) {
          return deferReadCallback(Error::STREAM_INVALID_ARGUMENT, std::move(cb));
        }
        reading_.emplace(Reading{out, std::move(cb)});
        return;
      }

      if (is_readable_) {
        feedback_.ackReceivedBytes(stream_id_, consumed);
      }
      return deferReadCallback(consumed, std::move(cb));
    }

    if (close_reason_) {
      return deferReadCallback(*close_reason_, std::move(cb));
    }

    if (reading_.has_value()) {
      abort();
    }

    if (!is_readable_) {
      // half closed
      return deferReadCallback(Error::STREAM_NOT_READABLE, std::move(cb));
    }

    reading_.emplace(Reading{out, std::move(cb)});
  }

  void YamuxStream::doWrite() {
    bool finish_close = false;
    {
      std::unique_lock lock(stream_write_mu_);
      finish_close = doWriteUnlocked();
    }
    if (finish_close) {
      doClose(Error::STREAM_CLOSED_BY_HOST);
    }
  }

  bool YamuxStream::doWriteUnlocked() {
    size_t initial_window_size = window_size_;

    BytesIn data;
    while (!close_reason_) {
      window_size_ = write_queue_.dequeue(window_size_, data);
      if (data.empty()) {
        break;
      }
      TRACE("stream {} dequeued {}/{} bytes to write",
            stream_id_,
            data.size(),
            write_queue_.unsentBytes() + data.size());
      // writeStreamData only enqueues on the connection (async); must not
      // re-enter this stream's write path while stream_write_mu_ is held.
      feedback_.writeStreamData(stream_id_, data);
    }

    if (initial_window_size != window_size_) {
      TRACE("stream {} send window size reduced from {} to {}",
            stream_id_,
            initial_window_size,
            window_size_);
    }

    if (!is_writable_ && !close_reason_ && window_size_ > 0) {
      // closing stream for writes, sends FIN
      if (!fin_sent_) {
        fin_sent_ = true;
        feedback_.streamClosed(stream_id_);
      }

      if (!is_readable_) {
        return true;  // caller unlocks then doClose
      }
      // let bytes be consumed with peers FIN even if no reader (???)
      peers_window_size_ = maximum_window_size_;
    }
    return false;
  }

  void YamuxStream::doWrite(BytesIn in, WriteCallbackFunc cb) {
    bool finish_close = false;
    {
      std::unique_lock lock(stream_write_mu_);
      if (in.empty()) {
        return deferWriteCallback(Error::STREAM_INVALID_ARGUMENT, std::move(cb));
      }

      if (!is_writable_) {
        return deferWriteCallback(Error::STREAM_NOT_WRITABLE, std::move(cb));
      }

      if (close_reason_) {
        return deferWriteCallback(
            std::error_code{},
            [cb{std::move(cb)}, res{*close_reason_}](
                outcome::result<size_t>) mutable { cb(std::move(res)); });
      }

      if (!write_queue_.canEnqueue(in.size())) {
        return deferWriteCallback(Error::STREAM_WRITE_OVERFLOW, std::move(cb));
      }

      write_queue_.enqueue(in, std::move(cb));
      finish_close = doWriteUnlocked();
    }
    if (finish_close) {
      doClose(Error::STREAM_CLOSED_BY_HOST);
    }
  }

}  // namespace libp2p::connection
