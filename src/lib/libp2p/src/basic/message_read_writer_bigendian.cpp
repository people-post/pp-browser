/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/basic/message_read_writer_bigendian.hpp>

#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include <cassert>
#include <libp2p/basic/message_read_writer_error.hpp>
#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/common/byteutil.hpp>

namespace libp2p::basic {
  MessageReadWriterBigEndian::MessageReadWriterBigEndian(
      std::shared_ptr<ReadWriter> conn)
      : conn_{std::move(conn)} {
    assert(conn_ != nullptr);
  }

  void MessageReadWriterBigEndian::read(ReadCallbackFunc cb) {
    auto buffer = std::make_shared<std::vector<uint8_t>>();
    buffer->resize(kLenMarkerSize);
    libp2p::read(conn_,
                 *buffer,
                 [self{shared_from_this()}, buffer, cb{std::move(cb)}](
                     outcome::result<void> result) {
                   if (result.has_error()) {
                     return cb(result.error());
                   }
                   uint32_t msg_len = ntohl(  // NOLINT
                       common::convert<uint32_t>(buffer->data()));
                   buffer->resize(msg_len);
                   std::fill(buffer->begin(), buffer->end(), 0u);
                   libp2p::read(
                       self->conn_,
                       *buffer,
                       [self, buffer, cb](outcome::result<void> result) {
                         if (result.has_error()) {
                           return cb(result.error());
                         }
                         cb(buffer);
                       });
                 });
  }

  void MessageReadWriterBigEndian::write(BytesIn buffer, CbOutcomeVoid cb) {
    if (buffer.empty()) {
      // TODO(107): Reentrancy
      cb(std::make_error_code(std::errc::invalid_argument));
      return;
    }

    std::vector<uint8_t> raw_buf;
    raw_buf.reserve(kLenMarkerSize + buffer.size());
    common::putUint32BE(raw_buf, buffer.size());
    raw_buf.insert(raw_buf.end(), buffer.begin(), buffer.end());
    libp2p::write(conn_,
                  raw_buf,
                  [self{shared_from_this()},
                   cb{std::move(cb)}](outcome::result<void> result) {
                    if (result.has_error()) {
                      return cb(result.error());
                    }
                    cb(outcome::success());
                  });
  }
}  // namespace libp2p::basic
