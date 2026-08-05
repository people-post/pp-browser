/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/noise/handshake_message_marshaller_impl.hpp>

#include <libp2p/wire/noise_wire.hpp>
OUTCOME_CPP_DEFINE_CATEGORY(libp2p::security::noise,
                            HandshakeMessageMarshallerImpl::Error,
                            e) {
  using E = libp2p::security::noise::HandshakeMessageMarshallerImpl::Error;
  switch (e) {
    case E::MESSAGE_SERIALIZING_ERROR:
      return "Unable to serialize handshake payload message to wire";
    case E::MESSAGE_DESERIALIZING_ERROR:
      return "Unable to deserialize handshake payload message from wire";
  }
  return "Unknown error";
}

namespace libp2p::security::noise {

  HandshakeMessageMarshallerImpl::HandshakeMessageMarshallerImpl(
      std::shared_ptr<crypto::marshaller::KeyMarshaller> marshaller)
      : marshaller_{std::move(marshaller)} {};

  outcome::result<wire::NoiseHandshakePayloadWire>
  HandshakeMessageMarshallerImpl::handyToWire(
      const HandshakeMessage &msg) const {
    wire::NoiseHandshakePayloadWire wire_msg;

    auto proto_pubkey_bytes_res = marshaller_->marshal(msg.identity_key);
    if (not proto_pubkey_bytes_res) {
      return std::move(proto_pubkey_bytes_res).as_failure();
    }
    auto proto_pubkey_bytes = std::move(proto_pubkey_bytes_res).value();
    wire_msg.identity_key.assign(proto_pubkey_bytes.key.begin(),
                                 proto_pubkey_bytes.key.end());
    wire_msg.identity_sig.assign(msg.identity_sig.begin(),
                                 msg.identity_sig.end());
    wire_msg.data.assign(msg.data.begin(), msg.data.end());
    return wire_msg;
  }

  outcome::result<std::pair<HandshakeMessage, crypto::ProtobufKey>>
  HandshakeMessageMarshallerImpl::wireToHandy(
      const wire::NoiseHandshakePayloadWire &wire_msg) const {
    crypto::ProtobufKey proto_key{wire_msg.identity_key};
    auto pubkey_res = marshaller_->unmarshalPublicKey(proto_key);
    if (not pubkey_res) {
      return std::move(pubkey_res).as_failure();
    }
    auto pubkey = std::move(pubkey_res).value();

    return std::make_pair(
        HandshakeMessage{
            .identity_key = std::move(pubkey),
            .identity_sig = wire_msg.identity_sig,
            .data = wire_msg.data},
        std::move(proto_key));
  }

  outcome::result<Bytes> HandshakeMessageMarshallerImpl::marshal(
      const HandshakeMessage &msg) const {
    auto wire_msg_res = handyToWire(msg);
    if (not wire_msg_res) {
      return std::move(wire_msg_res).as_failure();
    }
    auto encoded = wire_msg_res.value().encode();
    if (!encoded) {
      return Error::MESSAGE_SERIALIZING_ERROR;
    }
    return std::move(encoded.value());
  }

  outcome::result<std::pair<HandshakeMessage, crypto::ProtobufKey>>
  HandshakeMessageMarshallerImpl::unmarshal(BytesIn msg_bytes) const {
    auto wire_msg_res = wire::NoiseHandshakePayloadWire::decode(msg_bytes);
    if (!wire_msg_res) {
      return Error::MESSAGE_DESERIALIZING_ERROR;
    }
    return wireToHandy(wire_msg_res.value());
  }
}  // namespace libp2p::security::noise
