/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/secio/propose_message_marshaller_impl.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::security::secio,
                            ProposeMessageMarshallerImpl::Error,
                            e) {
  using E = libp2p::security::secio::ProposeMessageMarshallerImpl::Error;
  switch (e) {
    case E::MESSAGE_SERIALIZING_ERROR:
      return "Error while encoding SECIO Propose message";
    case E::MESSAGE_DESERIALIZING_ERROR:
      return "Error while decoding SECIO Propose message";
  }
  return "Unknown error";
}

namespace libp2p::security::secio {

  wire::SecioProposeWire ProposeMessageMarshallerImpl::handyToWire(
      const ProposeMessage &msg) const {
    wire::SecioProposeWire wire_msg;
    wire_msg.rand = msg.rand;
    wire_msg.pubkey = msg.pubkey;
    wire_msg.exchanges = msg.exchanges;
    wire_msg.ciphers = msg.ciphers;
    wire_msg.hashes = msg.hashes;
    return wire_msg;
  }

  ProposeMessage ProposeMessageMarshallerImpl::wireToHandy(
      const wire::SecioProposeWire &wire_msg) const {
    return ProposeMessage{
        .rand = wire_msg.rand.value_or(Bytes{}),
        .pubkey = wire_msg.pubkey.value_or(Bytes{}),
        .exchanges = wire_msg.exchanges.value_or(std::string{}),
        .ciphers = wire_msg.ciphers.value_or(std::string{}),
        .hashes = wire_msg.hashes.value_or(std::string{})};
  }

  outcome::result<std::vector<uint8_t>> ProposeMessageMarshallerImpl::marshal(
      const ProposeMessage &msg) const {
    auto wire_msg = handyToWire(msg);
    auto encoded = wire_msg.encode();
    if (!encoded) {
      return Error::MESSAGE_SERIALIZING_ERROR;
    }
    return std::move(encoded.value());
  }

  outcome::result<ProposeMessage> ProposeMessageMarshallerImpl::unmarshal(
      BytesIn msg_bytes) const {
    auto wire_msg_res = wire::SecioProposeWire::decode(msg_bytes);
    if (!wire_msg_res) {
      return Error::MESSAGE_DESERIALIZING_ERROR;
    }
    return wireToHandy(wire_msg_res.value());
  }

}  // namespace libp2p::security::secio
