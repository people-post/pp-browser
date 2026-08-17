/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/basic/message_read_writer.hpp>

namespace libp2p::basic {
  /**
   * Reader/writer for length-prefixed libp2p wire messages (hand-encoded protobuf wire).
   * User of this class MUST ENSURE that no two parallel reads or writes happen.
   */
  class WireMessageReadWriter
      : public std::enable_shared_from_this<WireMessageReadWriter> {
    template <typename Msg>
    using ReadCallbackFunc = std::function<void(outcome::result<Msg>)>;

   public:
    explicit WireMessageReadWriter(std::shared_ptr<MessageReadWriter> read_writer);
    explicit WireMessageReadWriter(std::shared_ptr<ReadWriter> conn);

    template <typename Msg>
    void read(ReadCallbackFunc<Msg> cb,
              const std::shared_ptr<std::vector<uint8_t>> &bytes = nullptr) {
      read_writer_->read(
          [self{shared_from_this()}, cb = std::move(cb), bytes](auto &&res) {
            if (!res) {
              return cb(res.error());
            }

            auto &&buf = res.value();
            if (!buf) {
              return cb(Msg{});
            }

            auto msg_res = Msg::decode(*buf);
            if (!msg_res) {
              return cb(msg_res.error());
            }
            if (bytes) {
              bytes->clear();
              bytes->swap(*buf);
            }
            cb(std::move(msg_res.value()));
          });
    }

    template <typename Msg>
    void write(const Msg &msg,
               CbOutcomeVoid cb,
               const std::shared_ptr<std::vector<uint8_t>> &bytes = nullptr) {
      auto encoded_res = msg.encode();
      if (!encoded_res) {
        return cb(encoded_res.error());
      }
      auto msg_bytes = std::make_shared<std::vector<uint8_t>>(
          std::move(encoded_res.value()));
      if (bytes) {
        bytes->clear();
        std::copy(msg_bytes->begin(), msg_bytes->end(), std::back_inserter(*bytes));
      }
      read_writer_->write(*msg_bytes,
                          [cb = std::move(cb), msg_bytes](auto &&res) {
                            cb(std::forward<decltype(res)>(res));
                          });
    }

   private:
    std::shared_ptr<MessageReadWriter> read_writer_;
  };

  using ProtobufMessageReadWriter = WireMessageReadWriter;
}  // namespace libp2p::basic
