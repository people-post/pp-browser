/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/secio/exchange_message_marshaller_impl.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::security::secio,
                            ExchangeMessageMarshallerImpl::Error,
                            e) {
  using E = libp2p::security::secio::ExchangeMessageMarshallerImpl::Error;
  switch (e) {
    case E::MESSAGE_SERIALIZING_ERROR:
      return "Error while encoding SECIO Exchange message";
    case E::MESSAGE_DESERIALIZING_ERROR:
      return "Error while decoding SECIO Exchange message";
  }
  return "Unknown error";
}

namespace libp2p::security::secio {

  wire::SecioExchangeWire ExchangeMessageMarshallerImpl::handyToWire(
      const ExchangeMessage &msg) const {
    wire::SecioExchangeWire wire_msg;
    wire_msg.epubkey = msg.epubkey;
    wire_msg.signature = msg.signature;
    return wire_msg;
  }

  ExchangeMessage ExchangeMessageMarshallerImpl::wireToHandy(
      const wire::SecioExchangeWire &wire_msg) const {
    return ExchangeMessage{
        .epubkey = wire_msg.epubkey.value_or(Bytes{}),
        .signature = wire_msg.signature.value_or(Bytes{})};
  }

  outcome::result<std::vector<uint8_t>> ExchangeMessageMarshallerImpl::marshal(
      const ExchangeMessage &msg) const {
    auto wire_msg = handyToWire(msg);
    auto encoded = wire_msg.encode();
    if (!encoded) {
      return Error::MESSAGE_SERIALIZING_ERROR;
    }
    return std::move(encoded.value());
  }

  outcome::result<ExchangeMessage> ExchangeMessageMarshallerImpl::unmarshal(
      BytesIn msg_bytes) const {
    auto wire_msg_res = wire::SecioExchangeWire::decode(msg_bytes);
    if (!wire_msg_res) {
      return Error::MESSAGE_DESERIALIZING_ERROR;
    }
    return wireToHandy(wire_msg_res.value());
  }

}  // namespace libp2p::security::secio
