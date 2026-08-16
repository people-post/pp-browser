/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <utility>

#include <libp2p/crypto/protobuf/protobuf_key.hpp>
#include <libp2p/outcome/outcome.hpp>
#include <libp2p/security/plaintext/exchange_message.hpp>
#include <libp2p/wire/plaintext_wire.hpp>

namespace libp2p::security::plaintext {

  class ExchangeMessageMarshaller {
   public:
    virtual ~ExchangeMessageMarshaller() = default;

    virtual outcome::result<wire::PlaintextExchangeWire> handyToWire(
        const ExchangeMessage &msg) const = 0;

    virtual outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>
    wireToHandy(const wire::PlaintextExchangeWire &wire_msg) const = 0;

    virtual outcome::result<std::vector<uint8_t>> marshal(
        const ExchangeMessage &msg) const = 0;

    virtual outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>
    unmarshal(BytesIn msg_bytes) const = 0;
  };

}  // namespace libp2p::security::plaintext
