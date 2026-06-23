/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/security/plaintext/exchange_message_marshaller_impl.hpp>

#include <generated/security/plaintext/protobuf/plaintext.pb.h>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::security::plaintext,
                            ExchangeMessageMarshallerImpl::Error,
                            e) {
  using E = libp2p::security::plaintext::ExchangeMessageMarshallerImpl::Error;
  switch (e) {
    case E::PUBLIC_KEY_SERIALIZING_ERROR:
      return "Error while encoding the public key to Protobuf format";
    case E::MESSAGE_SERIALIZING_ERROR:
      return "Error while encoding the plaintext exchange message to Protobuf "
             "format";
    case E::PUBLIC_KEY_DESERIALIZING_ERROR:
      return "Error while decoding the public key from Protobuf format";
    case E::MESSAGE_DESERIALIZING_ERROR:
      return "Error while decoding the plaintext exchange message from "
             "Protobuf format";
  }
  return "Unknown error";
}

namespace libp2p::security::plaintext {

  ExchangeMessageMarshallerImpl::ExchangeMessageMarshallerImpl(
      std::shared_ptr<crypto::marshaller::KeyMarshaller> marshaller)
      : marshaller_{std::move(marshaller)} {};

  outcome::result<protobuf::Exchange>
  ExchangeMessageMarshallerImpl::handyToProto(
      const ExchangeMessage &msg) const {
    plaintext::protobuf::Exchange exchange_msg;

    auto proto_pubkey_bytes_res = marshaller_->marshal(msg.pubkey);
    if (!proto_pubkey_bytes_res) {
      return proto_pubkey_bytes_res.as_failure();
    }
    auto proto_pubkey_bytes = std::move(proto_pubkey_bytes_res).value();
    if (!exchange_msg.mutable_pubkey()->ParseFromArray(
            proto_pubkey_bytes.key.data(), proto_pubkey_bytes.key.size())) {
      return Error::PUBLIC_KEY_SERIALIZING_ERROR;
    }

    auto id = msg.peer_id.toMultihash().toBuffer();
    exchange_msg.set_id(id.data(), id.size());

    return outcome::success(std::move(exchange_msg));
  }

  outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>
  ExchangeMessageMarshallerImpl::protoToHandy(
      const protobuf::Exchange &proto_msg) const {
    std::vector<uint8_t> pubkey_message_bytes(
        proto_msg.pubkey().ByteSizeLong());
    if (!proto_msg.pubkey().SerializeToArray(pubkey_message_bytes.data(),
                                             pubkey_message_bytes.size())) {
      return Error::PUBLIC_KEY_SERIALIZING_ERROR;
    }
    crypto::ProtobufKey proto_pubkey{pubkey_message_bytes};
    auto pubkey_res = marshaller_->unmarshalPublicKey(proto_pubkey);
    if (!pubkey_res) {
      return pubkey_res.as_failure();
    }
    auto pubkey = std::move(pubkey_res).value();

    std::vector<uint8_t> peer_id_bytes(proto_msg.id().begin(),
                                       proto_msg.id().end());
    auto peer_id_res = peer::PeerId::fromBytes(peer_id_bytes);
    if (!peer_id_res) {
      return peer_id_res.as_failure();
    }
    auto peer_id = std::move(peer_id_res).value();

    return {ExchangeMessage{.pubkey = pubkey, .peer_id = peer_id},
            proto_pubkey};
  }

  outcome::result<std::vector<uint8_t>> ExchangeMessageMarshallerImpl::marshal(
      const ExchangeMessage &msg) const {
    auto exchange_msg_res = handyToProto(msg);
    if (!exchange_msg_res) {
      return exchange_msg_res.as_failure();
    }
    auto exchange_msg = std::move(exchange_msg_res).value();

    std::vector<uint8_t> out_msg(exchange_msg.ByteSizeLong());
    if (!exchange_msg.SerializeToArray(out_msg.data(), out_msg.size())) {
      return Error::MESSAGE_SERIALIZING_ERROR;
    }
    return out_msg;
  }

  outcome::result<std::pair<ExchangeMessage, crypto::ProtobufKey>>
  ExchangeMessageMarshallerImpl::unmarshal(BytesIn msg_bytes) const {
    plaintext::protobuf::Exchange exchange_msg;
    if (!exchange_msg.ParseFromArray(msg_bytes.data(), msg_bytes.size())) {
      return Error::PUBLIC_KEY_DESERIALIZING_ERROR;
    }

    return protoToHandy(exchange_msg);
  }

}  // namespace libp2p::security::plaintext
