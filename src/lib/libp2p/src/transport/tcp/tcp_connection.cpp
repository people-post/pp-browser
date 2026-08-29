/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cassert>
#include <libp2p/transport/tcp/tcp_connection.hpp>

#include <libp2p/common/asio_buffer.hpp>
#include <libp2p/transport/tcp/bytes_counter.hpp>
#include <libp2p/transport/tcp/tcp_util.hpp>

#define TRACE_ENABLED 0
#include <libp2p/common/trace.hpp>

namespace libp2p::transport {

  namespace {
    auto &log() {
      static auto logger = log::createLogger("TcpConnection");
      return *logger;
    }
  }  // namespace

  TcpConnection::TcpConnection(asio::io_context &ctx,
                               ProtoAddrVec layers,
                               asio::ip::tcp::socket &&socket)
      : context_(ctx),
        layers_{std::move(layers)},
        socket_(std::move(socket)),
        connection_phase_done_{false},
        connect_timer_(context_) {
    std::ignore = saveMultiaddresses();
  }

  TcpConnection::TcpConnection(asio::io_context &ctx,
                               ProtoAddrVec layers)
      : context_(ctx),
        layers_{std::move(layers)},
        socket_(context_),
        connection_phase_done_{false},
        connect_timer_(context_) {}

  outcome::result<void> TcpConnection::close() {
    closed_by_host_ = true;
    close(make_error_code(std::errc::connection_aborted));
    return outcome::success();
  }

  void TcpConnection::close(std::error_code reason) {
    if (!close_reason_) {
      close_reason_ = reason;
      log().debug("{} closing with reason: {}", debug_str_, *close_reason_);
    }
    if (socket_.is_open()) {
      std::error_code ec;
      socket_.close(ec);
    }
  }

  bool TcpConnection::isClosed() const {
    return closed_by_host_ || !socket_.is_open();
  }

  outcome::result<multi::Multiaddress> TcpConnection::remoteMultiaddr() {
    if (!remote_multiaddress_) {
      auto res = saveMultiaddresses();
      if (!res) {
        return res.error();
      }
    }
    return remote_multiaddress_.value();
  }

  outcome::result<multi::Multiaddress> TcpConnection::localMultiaddr() {
    if (!local_multiaddress_) {
      auto res = saveMultiaddresses();
      if (!res) {
        return res.error();
      }
    }
    return local_multiaddress_.value();
  }

  bool TcpConnection::isInitiator() const {
    return initiator_;
  }

  namespace {
    template <typename Callback>
    auto closeOnError(TcpConnection &conn, Callback cb) {
      return [cb{std::move(cb)}, wptr{conn.weak_from_this()}](auto ec,
                                                              size_t result) {
        if (ec) {
          cb(ec);
          if (auto self = wptr.lock()) {
            self->close(ec);
          }
        } else {
          cb(result);
        }
      };
    }
  }  // namespace

  void TcpConnection::connect(
      const TcpConnection::ResolverResultsType &iterator,
      TcpConnection::ConnectCallbackFunc cb) {
    connect(iterator, std::move(cb), std::chrono::milliseconds::zero());
  }

  void TcpConnection::connect(
      const TcpConnection::ResolverResultsType &iterator,
      ConnectCallbackFunc cb,
      std::chrono::milliseconds timeout) {
    if (timeout > std::chrono::milliseconds::zero()) {
      connecting_with_timeout_ = true;
      connect_timer_.expires_after(timeout);
      connect_timer_.async_wait(
          [wptr{weak_from_this()}, cb](const std::error_code &error) {
            auto self = wptr.lock();
            if (!self || self->closed_by_host_) {
              return;
            }
            bool expected = false;
            if (self->connection_phase_done_.compare_exchange_strong(expected,
                                                                     true)) {
              if (not error) {
                // timeout happened, timer expired before connection was
                // established
                cb(std::make_error_code(std::errc::timed_out), Tcp::endpoint{});
              }
              // Another case is: asio::error::operation_aborted == error
              // connection was established before timeout and timer has been
              // cancelled
            }
          });
    }
    asio::async_connect(
        socket_,
        iterator,
        [wptr{weak_from_this()}, cb{std::move(cb)}](
            auto &&ec, const Tcp::endpoint &endpoint) {
          auto self = wptr.lock();
          if (!self || self->closed_by_host_) {
            return;
          }
          bool expected = false;
          if (not self->connection_phase_done_.compare_exchange_strong(expected,
                                                                       true)) {
            assert(expected);
            // connection phase already done - means that user's callback was
            // already called by timer expiration so we are closing socket if
            // it was actually connected
            if (not ec) {
              self->socket_.close();
            }
            return;
          }
          if (self->connecting_with_timeout_) {
            self->connect_timer_.cancel();
          }
          self->initiator_ = true;
          std::ignore = self->saveMultiaddresses();
          cb(std::forward<decltype(ec)>(ec),
             std::forward<decltype(endpoint)>(endpoint));
        });
  }

  void TcpConnection::readSome(BytesOut out,
                               TcpConnection::ReadCallbackFunc cb) {
    ByteCounter::getInstance().incrementBytesRead(out.size());
    TRACE("{} read some up to {}", debug_str_, out.size());
    socket_.async_read_some(asioBuffer(out),
                            closeOnError(*this, std::move(cb)));
  }

  void TcpConnection::writeSome(BytesIn in,
                                TcpConnection::WriteCallbackFunc cb) {
    ByteCounter::getInstance().incrementBytesWritten(in.size());
    TRACE("{} write some up to {}", debug_str_, in.size());
    socket_.async_write_some(asioBuffer(in),
                             closeOnError(*this, std::move(cb)));
  }

  void TcpConnection::deferReadCallback(outcome::result<size_t> res,
                                        ReadCallbackFunc cb) {
    asio::post(context_, [res, cb{std::move(cb)}] { cb(res); });
  }

  void TcpConnection::deferWriteCallback(std::error_code ec,
                                         WriteCallbackFunc cb) {
    asio::post(context_, [ec, cb{std::move(cb)}] { cb(ec); });
  }

  outcome::result<void> TcpConnection::saveMultiaddresses() {
    std::error_code ec;
    if (socket_.is_open()) {
      if (!local_multiaddress_) {
        auto endpoint(socket_.local_endpoint(ec));
        if (!ec) {
          auto local_multiaddress_res = detail::makeAddress(endpoint, layers_);
          if (!local_multiaddress_res) {
            return std::move(local_multiaddress_res).as_failure();
          }
          local_multiaddress_ = std::move(local_multiaddress_res).value();
        }
      }
      if (!remote_multiaddress_) {
        auto endpoint(socket_.remote_endpoint(ec));
        if (!ec) {
          auto remote_multiaddress_res = detail::makeAddress(endpoint, layers_);
          if (!remote_multiaddress_res) {
            return std::move(remote_multiaddress_res).as_failure();
          }
          remote_multiaddress_ = std::move(remote_multiaddress_res).value();
        }
      }
    } else {
      return make_error_code(std::errc::not_connected);
    }
    if (ec) {
      return ec;
    }
#ifndef NDEBUG
    debug_str_ = fmt::format("{} {} {}",
                             local_multiaddress_->getStringAddress(),
                             initiator_ ? "->" : "<-",
                             remote_multiaddress_->getStringAddress());
#endif
    return outcome::success();
  }

  uint64_t TcpConnection::getBytesRead() {
    return ByteCounter::getInstance().getBytesRead();
  }

  uint64_t TcpConnection::getBytesWritten() {
    return ByteCounter::getInstance().getBytesWritten();
  }

}  // namespace libp2p::transport
