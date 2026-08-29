/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <cassert>
#include <libp2p/protocol/kademlia/impl/content_routing_table_impl.hpp>

namespace libp2p::protocol::kademlia {

  ContentRoutingTableImpl::ContentRoutingTableImpl(
      const Config &config,
      basic::Scheduler &scheduler,
      std::shared_ptr<event::Bus> bus)
      : config_(config), scheduler_(scheduler), bus_(std::move(bus)) {
    assert(bus_ != nullptr);
  }

  void ContentRoutingTableImpl::start() {
    setTimerCleanup();
  }

  ContentRoutingTableImpl::~ContentRoutingTableImpl() = default;

  std::vector<PeerId> ContentRoutingTableImpl::getProvidersFor(
      const ContentId &key, size_t limit) const {
    std::vector<PeerId> result;
    auto it = table_.find(key);
    if (it == table_.end()) {
      return result;
    }
    for (const auto &provider : it->second) {
      result.push_back(provider.peer);
      if (limit > 0 and result.size() >= limit) {
        break;
      }
    }
    return result;
  }

  void ContentRoutingTableImpl::addProvider(const ContentId &key,
                                            const peer::PeerId &peer) {
    auto expires = scheduler_.now() + config_.providerRecordTTL;
    auto &providers = table_[key];
    auto equal = providers.end();
    auto oldest = providers.begin();
    for (auto it = providers.begin(); it != providers.end(); ++it) {
      if (it->peer == peer) {
        equal = it;
        break;
      }
      if (oldest == providers.end()
          || it->expire_time < oldest->expire_time) {
        oldest = it;
      }
    }
    if (equal != providers.end()) {
      // provider refreshed itself, so do our host
      equal->expire_time = expires;
      return;
    }
    if (providers.size() >= config_.maxProvidersPerKey && !providers.empty()) {
      providers.erase(oldest);
    }
    providers.push_back(Provider{peer, expires});
    bus_->getChannel<event::protocol::kademlia::ProvideContentChannel>()
        .publish({key, peer});
  }

  void ContentRoutingTableImpl::onCleanupTimer() {
    auto current_time = scheduler_.now();

    for (auto it = table_.begin(); it != table_.end();) {
      auto &providers = it->second;
      providers.erase(std::remove_if(providers.begin(),
                                     providers.end(),
                                     [current_time](const Provider &p) {
                                       return p.expire_time <= current_time;
                                     }),
                      providers.end());
      if (providers.empty()) {
        it = table_.erase(it);
      } else {
        ++it;
      }
    }

    setTimerCleanup();
  }

  void ContentRoutingTableImpl::setTimerCleanup() {
    cleanup_timer_ = scheduler_.scheduleWithHandle(
        [weak_self{weak_from_this()}] {
          auto self = weak_self.lock();
          if (not self) {
            return;
          }
          self->onCleanupTimer();
        },
        config_.providerWipingInterval);
  }
}  // namespace libp2p::protocol::kademlia
