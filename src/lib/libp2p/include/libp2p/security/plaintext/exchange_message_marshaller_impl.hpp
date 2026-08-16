/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/crypto/key_marshaller.hpp>
#include <libp2p/security/plaintext/exchange_message_marshaller.hpp>

namespace libp2p::security::plaintext {

  class ExchangeMessageMarshallerImpl : public ExchangeMessageMarshaller {
   public:
    enum class Error {
      PUBLIC_KEY_SERIALIZING_ERROR = 1,
      MESSAGE_SERIALIZING_ERROR = 2,
      PUBLIC_KEY_DESERIALIZING_ERROR = 3,
      MESSAGE_DESERIALIZING_ERROR = 4,
    };

    explicit ExchangeMessageMarshallerImpl(
        std::shared_ptr<crypto::marshaller::KeyMarshaller> marshaller);

    outcome::result<wire::PlaintextExchangeWire> handyToWire(
        const ExchangeMessage &msg) const override;

    outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>
    wireToHandy(const wire::PlaintextExchangeWire &wire_msg) const override;

    outcome::result<std::vector<uint8_t>> marshal(
        const ExchangeMessage &msg) const override;

    outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>
    unmarshal(BytesIn msg_bytes) const override;

   private:
    std::shared_ptr<crypto::marshaller::KeyMarshaller> marshaller_;
  };

}  // namespace libp2p::security::plaintext

OUTCOME_HPP_DECLARE_ERROR(libp2p::security::plaintext,
                          ExchangeMessageMarshallerImpl::Error);
