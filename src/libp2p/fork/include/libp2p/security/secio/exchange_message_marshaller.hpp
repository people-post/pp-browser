/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/outcome/outcome.hpp>
#include <libp2p/security/secio/exchange_message.hpp>
#include <libp2p/wire/secio_wire.hpp>

namespace libp2p::security::secio {

  class ExchangeMessageMarshaller {
   public:
    virtual ~ExchangeMessageMarshaller() = default;

    virtual wire::SecioExchangeWire handyToWire(
        const ExchangeMessage &msg) const = 0;

    virtual ExchangeMessage wireToHandy(
        const wire::SecioExchangeWire &wire_msg) const = 0;

    virtual outcome::result<std::vector<uint8_t>> marshal(
        const ExchangeMessage &msg) const = 0;

    virtual outcome::result<ExchangeMessage> unmarshal(
        BytesIn msg_bytes) const = 0;
  };
}  // namespace libp2p::security::secio
