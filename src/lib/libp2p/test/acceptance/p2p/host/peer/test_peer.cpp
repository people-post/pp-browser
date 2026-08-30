/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include "acceptance/p2p/host/peer/test_peer.hpp"

#include <asio/post.hpp>
#include <gtest/gtest.h>
#include <qtils/test/outcome.hpp>

#include "acceptance/p2p/host/peer/tick_counter.hpp"
#include "acceptance/p2p/host/protocol/client_test_session.hpp"

using namespace libp2p;  // NOLINT

Peer::Peer(Peer::Duration timeout, bool secure)
    : muxed_config_{1024576, 1000},
      timeout_{timeout},
      context_{std::make_shared<Context>()},
      echo_{std::make_shared<Echo>()} {
  const auto security =
      secure ? HostSecurityKind::Noise : HostSecurityKind::Plaintext;
  host_ = createExplicitHost(context_,
                             HostMuxerKind::Yamux,
                             security,
                             std::nullopt,
                             muxed_config_);

  auto handler = [this](StreamAndProtocol stream) { echo_->handle(stream); };
  host_->setProtocolHandler({echo_->getProtocolId()}, handler);
}

void Peer::startServer(const multi::Multiaddress &address,
                       std::shared_ptr<std::promise<peer::PeerInfo>> promise) {
  asio::post(*context_, [this, address, p = std::move(promise)] {
    ASSERT_OUTCOME_SUCCESS(host_->listen(address));
    host_->start();
    p->set_value(host_->getPeerInfo());
  });

  thread_ = std::thread([this] { context_->run_for(timeout_); });
}

void Peer::startClient(const peer::PeerInfo &pinfo,
                       size_t message_count,
                       Peer::sptr<TickCounter> counter) {
  asio::post(*context_,
             [this,
              server_id = pinfo.id.toBase58(),
              pinfo,
              message_count,
              counter = std::move(counter)]() mutable {
               this->host_->newStream(
                   pinfo,
                   {echo_->getProtocolId()},
                   [server_id = std::move(server_id),
                    ping_times = message_count,
                    counter = std::move(counter)](
                       StreamAndProtocolOrError rstream) mutable {
                     // get stream
                     if (rstream) {
                       auto stream = std::move(rstream.value());
                       // make client session
                       auto client = std::make_shared<protocol::ClientTestSession>(
                           stream.stream, ping_times);
                       // handle session
                       client->handle(
                           [server_id = std::move(server_id),
                            client,
                            counter = std::move(counter)](
                               outcome::result<std::vector<uint8_t>> res) mutable {
                             // count message exchange
                             counter->tick();
                             // ensure message returned
                             if (res) {
                               auto vec = std::move(res.value());
                               // ensure message is correct
                               ASSERT_EQ(vec.size(), client->bufferSize());  // NOLINT
                             } else {
                               FAIL() << "Failed to get message result";
                             }
                           });
                     } else {
                       FAIL() << "Failed to create stream";
                     }
                   });
             });
}

void Peer::wait() {
  if (thread_.joinable()) {
    thread_.join();
  }
  host_->stop();
}
