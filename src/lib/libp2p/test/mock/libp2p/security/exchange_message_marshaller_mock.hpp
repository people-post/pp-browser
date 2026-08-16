/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <gmock/gmock.h>
#include <libp2p/security/plaintext/exchange_message.hpp>
#include <libp2p/security/plaintext/exchange_message_marshaller.hpp>
#include <libp2p/wire/plaintext_wire.hpp>

namespace libp2p::security::plaintext {

  class ExchangeMessageMarshallerMock : public ExchangeMessageMarshaller {
   public:
    MOCK_METHOD(outcome::result<wire::PlaintextExchangeWire>,
                handyToWire,
                (const ExchangeMessage &),
                (const, override));
    MOCK_METHOD((outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>),
                wireToHandy,
                (const wire::PlaintextExchangeWire &),
                (const, override));
    MOCK_METHOD(outcome::result<std::vector<uint8_t>>,
                marshal,
                (const ExchangeMessage &),
                (const, override));
    MOCK_METHOD((outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>),
                unmarshal,
                (BytesIn),
                (const, override));
  };

}  // namespace libp2p::security::plaintext
