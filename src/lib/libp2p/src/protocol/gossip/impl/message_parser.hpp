/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "common.hpp"

#include <libp2p/wire/gossip_wire.hpp>

namespace libp2p::protocol::gossip {

  class MessageReceiver;

  class MessageParser {
   public:
    MessageParser();

    ~MessageParser();

    bool parse(BytesIn bytes);

    void dispatch(const PeerContextPtr &from, MessageReceiver &receiver);

   private:
    wire::GossipRpcWire wire_msg_;
  };

}  // namespace libp2p::protocol::gossip
