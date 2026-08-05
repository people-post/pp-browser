/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/outcome/outcome.hpp>
#include <libp2p/security/secio/propose_message.hpp>
#include <libp2p/wire/secio_wire.hpp>

namespace libp2p::security::secio {

  class ProposeMessageMarshaller {
   public:
    virtual ~ProposeMessageMarshaller() = default;

    virtual wire::SecioProposeWire handyToWire(
        const ProposeMessage &msg) const = 0;

    virtual ProposeMessage wireToHandy(
        const wire::SecioProposeWire &wire_msg) const = 0;

    virtual outcome::result<std::vector<uint8_t>> marshal(
        const ProposeMessage &msg) const = 0;

    virtual outcome::result<ProposeMessage> unmarshal(
        BytesIn msg_bytes) const = 0;
  };

}  // namespace libp2p::security::secio
