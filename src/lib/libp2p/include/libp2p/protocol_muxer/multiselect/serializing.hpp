/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/common/static_vector.hpp>
#include <libp2p/protocol_muxer/protocol_muxer.hpp>

#include "common.hpp"

namespace libp2p::protocol_muxer::multiselect::detail {

  /// Static vector for temp msg crafting
  using TmpMsgBuf =
      libp2p::common::StaticVector<uint8_t, kMaxMessageSize + kMaxVarintSize>;

  /// Appends varint prefix to buffer
  template <typename Buffer>
  inline void appendVarint(Buffer &buffer, size_t size) {
    do {
      uint8_t byte = size & 0x7F;
      size >>= 7;
      if (size != 0) {
        byte |= 0x80;
      }
      buffer.push_back(byte);
    } while (size > 0);
  }

  /// Appends protocol message to buffer
  template <typename Buffer, typename String>
  inline outcome::result<void> appendProtocol(Buffer &buffer,
                                              const String &protocol) {
    auto msg_size = protocol.size() + 1;
    if (msg_size > kMaxMessageSize - kMaxVarintSize) {
      return ProtocolMuxer::Error::INTERNAL_ERROR;
    }
    appendVarint(buffer, msg_size);
    buffer.insert(buffer.end(), protocol.begin(), protocol.end());
    buffer.push_back(kNewLine);
    if (buffer.size() <= kMaxMessageSize) {
      return outcome::success();
    }
    return ProtocolMuxer::Error::INTERNAL_ERROR;
  }

  /// Creates simple protocol message (one string)
  template <typename String>
  inline outcome::result<MsgBuf> createMessage(const String &protocol) {
    MsgBuf ret;
    ret.reserve(protocol.size() + 1 + kMaxVarintSize);
    auto append_protocol_res = appendProtocol(ret, protocol);
    if (!append_protocol_res) {
      return append_protocol_res.as_failure();
    }
    return ret;
  }

  /// Appends varint-delimited protocol list to buffer
  template <typename Buffer, typename Container>
  inline outcome::result<void> appendProtocolList(Buffer &buffer,
                                                  const Container &protocols,
                                                  bool append_final_new_line) {
    try {
      for (const auto &p : protocols) {
        auto append_protocol_res = appendProtocol(buffer, p);
        if (!append_protocol_res) {
          return append_protocol_res.as_failure();
        }
      }

      if (append_final_new_line) {
        buffer.push_back(kNewLine);
      }

    } catch (const std::bad_alloc &e) {
      // static tmp buffer throws this on oversize
      return ProtocolMuxer::Error::INTERNAL_ERROR;
    }

    return outcome::success();
  }

  /// Creates complex protocol message (multiple strings)
  template <class Container>
  inline outcome::result<MsgBuf> createMessage(const Container &protocols,
                                               bool nested) {
    MsgBuf ret_buf;

    if (nested) {
      TmpMsgBuf tmp_buf;
      auto append_protocol_list_res =
          appendProtocolList(tmp_buf, protocols, true);
      if (!append_protocol_list_res) {
        return append_protocol_list_res.as_failure();
      }
      ret_buf.reserve(tmp_buf.size() + kMaxVarintSize);
      appendVarint(ret_buf, tmp_buf.size());
      ret_buf.insert(ret_buf.end(), tmp_buf.begin(), tmp_buf.end());
    } else {
      auto append_protocol_list_res =
          appendProtocolList(ret_buf, protocols, false);
      if (!append_protocol_list_res) {
        return append_protocol_list_res.as_failure();
      }
    }

    return ret_buf;
  }

}  // namespace libp2p::protocol_muxer::multiselect::detail
