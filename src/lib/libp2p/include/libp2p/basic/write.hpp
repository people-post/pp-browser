/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <asio/error.hpp>
#include <libp2p/basic/writer.hpp>
#include <memory>

namespace libp2p {
  /// Write exactly `in.size()` bytes
  inline void write(const std::shared_ptr<basic::Writer> &writer,
                    BytesIn in,
                    std::function<void(outcome::result<void>)> cb) {
    // write some bytes
    writer->writeSome(
        in,
        [weak{std::weak_ptr{writer}}, in, cb{std::move(cb)}](
            outcome::result<size_t> n_res) mutable {
          if (n_res.has_error()) {
            return cb(n_res.error());
          }
          auto n = n_res.value();
          if (n == in.size()) {
            // successfully wrote last bytes
            return cb(outcome::success());
          }
          // Never throw from asio/libp2p callbacks (same as libp2p::read).
          if (n == 0 || n > in.size()) {
            return cb(make_error_code(asio::error::invalid_argument));
          }
          // write remaining bytes
          auto writer = weak.lock();
          if (not writer) {
            return cb(make_error_code(asio::error::operation_aborted));
          }
          write(writer, in.subspan(n), std::move(cb));
        });
  }
}  // namespace libp2p
