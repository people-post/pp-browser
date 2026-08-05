/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/plaintext/exchange_message_marshaller_impl.hpp>

#include <libp2p/wire/keys_wire.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::security::plaintext,
                            ExchangeMessageMarshallerImpl::Error,
                            e) {
  using E = libp2p::security::plaintext::ExchangeMessageMarshallerImpl::Error;
  switch (e) {
    case E::PUBLIC_KEY_SERIALIZING_ERROR:
      return "Error while encoding the public key";
    case E::MESSAGE_SERIALIZING_ERROR:
      return "Error while encoding the plaintext exchange message";
    case E::PUBLIC_KEY_DESERIALIZING_ERROR:
      return "Error while decoding the public key";
    case E::MESSAGE_DESERIALIZING_ERROR:
      return "Error while decoding the plaintext exchange message";
  }
  return "Unknown error";
}

namespace libp2p::security::plaintext {

  ExchangeMessageMarshallerImpl::ExchangeMessageMarshallerImpl(
      std::shared_ptr<crypto::marshaller::KeyMarshaller> marshaller)
      : marshaller_{std::move(marshaller)} {};

  outcome::result<wire::PlaintextExchangeWire>
  ExchangeMessageMarshallerImpl::handyToWire(
      const ExchangeMessage &msg) const {
    wire::PlaintextExchangeWire exchange_msg;

    auto proto_pubkey_bytes_res = marshaller_->marshal(msg.pubkey);
    if (!proto_pubkey_bytes_res) {
      return proto_pubkey_bytes_res.as_failure();
    }
    auto proto_pubkey_bytes = std::move(proto_pubkey_bytes_res).value();
    auto pubkey_wire_res =
        wire::PublicKeyWire::decode(proto_pubkey_bytes.key);
    if (!pubkey_wire_res) {
      return Error::PUBLIC_KEY_SERIALIZING_ERROR;
    }
    exchange_msg.pubkey = std::move(pubkey_wire_res.value());

    auto id = msg.peer_id.toMultihash().toBuffer();
    exchange_msg.id = id;

    return outcome::success(std::move(exchange_msg));
  }

  outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>
  ExchangeMessageMarshallerImpl::wireToHandy(
      const wire::PlaintextExchangeWire &wire_msg) const {
    if (!wire_msg.pubkey) {
      return Error::PUBLIC_KEY_DESERIALIZING_ERROR;
    }
    auto encoded_key_res = wire_msg.pubkey->encode();
    if (!encoded_key_res) {
      return Error::PUBLIC_KEY_SERIALIZING_ERROR;
    }
    crypto::ProtobufKey proto_pubkey{std::move(encoded_key_res.value())};
    auto pubkey_res = marshaller_->unmarshalPublicKey(proto_pubkey);
    if (!pubkey_res) {
      return pubkey_res.as_failure();
    }
    auto pubkey = std::move(pubkey_res).value();

    if (!wire_msg.id) {
      return Error::MESSAGE_DESERIALIZING_ERROR;
    }
    auto peer_id_res = peer::PeerId::fromBytes(*wire_msg.id);
    if (!peer_id_res) {
      return peer_id_res.as_failure();
    }
    auto peer_id = std::move(peer_id_res).value();

    return {ExchangeMessage{.pubkey = pubkey, .peer_id = peer_id},
            proto_pubkey};
  }

  outcome::result<std::vector<uint8_t>> ExchangeMessageMarshallerImpl::marshal(
      const ExchangeMessage &msg) const {
    auto exchange_msg_res = handyToWire(msg);
    if (!exchange_msg_res) {
      return exchange_msg_res.as_failure();
    }
    auto encoded = exchange_msg_res.value().encode();
    if (!encoded) {
      return Error::MESSAGE_SERIALIZING_ERROR;
    }
    return std::move(encoded.value());
  }

  outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>
  ExchangeMessageMarshallerImpl::unmarshal(BytesIn msg_bytes) const {
    auto exchange_msg_res = wire::PlaintextExchangeWire::decode(msg_bytes);
    if (!exchange_msg_res) {
      return Error::PUBLIC_KEY_DESERIALIZING_ERROR;
    }

    return wireToHandy(exchange_msg_res.value());
  }

}  // namespace libp2p::security::plaintext
