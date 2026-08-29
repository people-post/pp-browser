/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/protocol/kademlia/impl/content_routing_table.hpp>

#include <unordered_map>
#include <vector>

#include <libp2p/basic/scheduler.hpp>
#include <libp2p/protocol/kademlia/common.hpp>
#include <libp2p/protocol/kademlia/config.hpp>

namespace libp2p::protocol::kademlia {

  class ContentRoutingTableImpl
      : public ContentRoutingTable,
        public std::enable_shared_from_this<ContentRoutingTableImpl> {
   public:
    ContentRoutingTableImpl(const Config &config,
                            basic::Scheduler &scheduler,
                            std::shared_ptr<event::Bus> bus);

    ~ContentRoutingTableImpl() override;

    void start() override;

    std::vector<PeerId> getProvidersFor(const ContentId &key,
                                        size_t limit = 0) const override;

    void addProvider(const ContentId &key, const peer::PeerId &peer) override;

   private:
    struct Provider {
      peer::PeerId peer;
      Time expire_time = Time::zero();
    };

    void onCleanupTimer();
    void setTimerCleanup();

    const Config &config_;
    basic::Scheduler &scheduler_;
    std::shared_ptr<event::Bus> bus_;
    std::unordered_map<ContentId, std::vector<Provider>> table_;
    basic::Scheduler::Handle cleanup_timer_;
  };

}  // namespace libp2p::protocol::kademlia
