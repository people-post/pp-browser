/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libp2p/protocol/kademlia/impl/response_handler.hpp>

#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <libp2p/common/types.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/log/sublogger.hpp>
#include <libp2p/protocol/kademlia/common.hpp>
#include <libp2p/protocol/kademlia/config.hpp>
#include <libp2p/protocol/kademlia/impl/content_routing_table.hpp>
#include <libp2p/protocol/kademlia/impl/executors_factory.hpp>
#include <libp2p/protocol/kademlia/impl/peer_id_with_distance.hpp>
#include <libp2p/protocol/kademlia/impl/peer_routing_table.hpp>
#include <libp2p/protocol/kademlia/impl/session.hpp>
#include <libp2p/protocol/kademlia/impl/session_host.hpp>
#include <libp2p/protocol/kademlia/validator.hpp>

namespace libp2p::protocol::kademlia {

  class GetValueExecutor
      : public ResponseHandler,
        public std::enable_shared_from_this<GetValueExecutor> {
   public:
    GetValueExecutor(
        const Config &config,
        std::shared_ptr<Host> host,
        std::shared_ptr<basic::Scheduler> scheduler,
        std::shared_ptr<SessionHost> session_host,
        std::shared_ptr<ContentRoutingTable> content_routing_table,
        const std::shared_ptr<PeerRoutingTable> &peer_routing_table,
        std::shared_ptr<ExecutorsFactory> executor_factory,
        std::shared_ptr<Validator> validator,
        ContentId key,
        FoundValueHandler handler);

    ~GetValueExecutor() override;

    outcome::result<void> start();

    /// @see ResponseHandler::responseTimeout
    Time responseTimeout() const override;

    /// @see ResponseHandler::match
    bool match(const Message &msg) const override;

    /// @see ResponseHandler::onResult
    void onResult(const std::shared_ptr<Session> &session,
                  outcome::result<Message> msg_res) override;

   private:
    /// Spawns new request
    void spawn();

    /// Handles result of connection
    void onConnected(StreamAndProtocolOrError stream_res);

    void finish();

    static std::atomic_size_t instance_number;

    // Primary
    const Config &config_;
    std::shared_ptr<Host> host_;
    std::shared_ptr<basic::Scheduler> scheduler_;
    std::shared_ptr<SessionHost> session_host_;
    std::shared_ptr<ContentRoutingTable> content_routing_table_;
    std::shared_ptr<ExecutorsFactory> executor_factory_;
    std::shared_ptr<Validator> validator_;
    const ContentId key_;
    FoundValueHandler handler_;

    // Secondary
    const NodeId target_;
    std::unordered_set<PeerId> nearest_peer_ids_;

    // Auxiliary
    std::shared_ptr<std::vector<uint8_t>> serialized_request_;
    std::priority_queue<PeerIdWithDistance> queue_;
    size_t requests_in_progress_ = 0;

    /// PeerId -> received value (unique by peer)
    std::unordered_map<PeerId, Value> received_records_;

    bool started_ = false;
    bool done_ = false;

    log::SubLogger log_;
  };

}  // namespace libp2p::protocol::kademlia
