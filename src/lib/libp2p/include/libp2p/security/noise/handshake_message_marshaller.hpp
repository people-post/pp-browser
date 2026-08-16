/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <tuple>

#include <libp2p/crypto/protobuf/protobuf_key.hpp>
#include <libp2p/security/noise/handshake_message.hpp>
#include <libp2p/wire/noise_wire.hpp>

namespace libp2p::security::noise {
  /**
   * Serializes and deserializes Noise handshake payload messages.
   */
  class HandshakeMessageMarshaller {
   public:
    virtual ~HandshakeMessageMarshaller() = default;

    virtual outcome::result<wire::NoiseHandshakePayloadWire> handyToWire(
        const HandshakeMessage &msg) const = 0;

    virtual outcome::result<std::pair<HandshakeMessage, crypto::ProtobufKey>>
    wireToHandy(const wire::NoiseHandshakePayloadWire &wire_msg) const = 0;

    virtual outcome::result<Bytes> marshal(
        const HandshakeMessage &msg) const = 0;

    virtual outcome::result<std::pair<HandshakeMessage, crypto::ProtobufKey>>
    unmarshal(BytesIn msg_bytes) const = 0;
  };
}  // namespace libp2p::security::noise
