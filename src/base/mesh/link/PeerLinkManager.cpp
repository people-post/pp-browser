#include "base/mesh/link/PeerLinkManager.h"

#include "base/mesh/link/AdpMultiaddr.h"
#include "base/mesh/link/Types.h"

namespace pbr::amp {

PeerLinkManager::PeerLinkManager(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                                 PeerLinkConfig config)
    : endpoint_(endpoint), local_identity_(std::move(local_identity)), local_peer_id_(std::move(local_peer_id)),
      config_(config) {
  endpoint_.SetAcceptKey(PreSessionPeerKey());
  InstallAcceptHandler();
}

void PeerLinkManager::InstallAcceptHandler() {
  endpoint_.SetAcceptHandler([this](std::shared_ptr<adp::Connection> connection) {
    OnInboundConnection(std::move(connection));
  });
}

Roe<void> PeerLinkManager::RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) {
  auto parsed = ParseAdpMultiaddr(multiaddr);
  if (!parsed) {
    return parsed.error();
  }
  EndpointRecord rec;
  rec.multiaddr = multiaddr;
  rec.endpoint = parsed->endpoint;
  rec.peer_id = parsed->peer_id;
  endpoints_[peer_key] = std::move(rec);
  return Roe<void>();
}

PeerLink* PeerLinkManager::FindLink(const std::string& peer_key) {
  auto it = links_.find(peer_key);
  if (it == links_.end()) {
    return nullptr;
  }
  return it->second.get();
}

const PeerLink* PeerLinkManager::FindLink(const std::string& peer_key) const {
  auto it = links_.find(peer_key);
  if (it == links_.end()) {
    return nullptr;
  }
  return it->second.get();
}

PeerLink* PeerLinkManager::FindLinkByPeerId(const std::string& peer_id) {
  if (peer_id.empty()) {
    return nullptr;
  }
  if (const auto it = peer_id_to_key_.find(peer_id); it != peer_id_to_key_.end()) {
    return FindLink(it->second);
  }
  for (auto& [key, link] : links_) {
    if (link->Phase() == PeerLinkPhase::Connected && link->RemotePeerId() == peer_id) {
      peer_id_to_key_[peer_id] = key;
      return link.get();
    }
  }
  return nullptr;
}

const PeerLink* PeerLinkManager::FindLinkByPeerId(const std::string& peer_id) const {
  return const_cast<PeerLinkManager*>(this)->FindLinkByPeerId(peer_id);
}

PeerLink* PeerLinkManager::FindConnectedLinkForPeerId(const std::string& peer_id) {
  if (auto* link = FindLinkByPeerId(peer_id)) {
    if (link->Phase() == PeerLinkPhase::Connected) {
      return link;
    }
  }
  return nullptr;
}

std::string PeerLinkManager::DeriveRemotePeerId(const ByteVector& identity_public_key) const {
  if (config_.peer_id_from_identity) {
    return config_.peer_id_from_identity(identity_public_key);
  }
  return IdentityPublicKeyFingerprint(identity_public_key);
}

PeerLink* PeerLinkManager::FindConnectedInboundLink() {
  for (auto& [_, link] : links_) {
    if (!link->IsOutbound() && link->Phase() == PeerLinkPhase::Connected) {
      return link.get();
    }
  }
  return nullptr;
}

bool PeerLinkManager::IsConnected(const std::string& peer_key) const {
  if (const auto* link = FindLink(peer_key)) {
    return link->Phase() == PeerLinkPhase::Connected;
  }
  return false;
}

PeerLinkSnapshot PeerLinkManager::GetLinkSnapshot(const std::string& peer_key) const {
  PeerLinkSnapshot snap;
  snap.has_endpoint = endpoints_.contains(peer_key);
  if (!snap.has_endpoint) {
    snap.phase = PeerLinkPhase::Unavailable;
    return snap;
  }
  snap.multiaddr = endpoints_.at(peer_key).multiaddr;
  if (const auto* link = FindLink(peer_key)) {
    snap.phase = link->Phase();
  } else if (const auto it = dial_failed_until_.find(peer_key); it != dial_failed_until_.end()) {
    const auto now = std::chrono::steady_clock::now();
    if (it->second > now) {
      snap.phase = PeerLinkPhase::Backoff;
      snap.backoff_remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now);
    } else {
      snap.phase = PeerLinkPhase::Idle;
    }
  } else {
    snap.phase = PeerLinkPhase::Idle;
  }
  if (const auto err = last_error_.find(peer_key); err != last_error_.end()) {
    snap.detail = err->second;
  }
  return snap;
}

void PeerLinkManager::EnsureAssociation(const std::string& peer_key, LinkCb on_complete) {
  if (IsConnected(peer_key)) {
    if (on_complete) {
      on_complete(Roe<void>());
    }
    return;
  }

  const auto ep_it = endpoints_.find(peer_key);
  if (ep_it != endpoints_.end()) {
    if (auto* existing = FindConnectedLinkForPeerId(ep_it->second.peer_id)) {
      if (existing->PeerKey() != peer_key) {
        RekeyLink(existing->PeerKey(), peer_key);
      }
      if (on_complete) {
        on_complete(Roe<void>());
      }
      return;
    }
  }

  if (auto* existing = FindLink(peer_key)) {
    if (existing->Phase() == PeerLinkPhase::Handshaking || existing->Phase() == PeerLinkPhase::Dialing) {
      inflight_associations_[peer_key].push_back(std::move(on_complete));
      return;
    }
  }

  if (ep_it == endpoints_.end()) {
    if (on_complete) {
      on_complete(Error("amp link: peer endpoint not registered"));
    }
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (const auto backoff = dial_failed_until_.find(peer_key); backoff != dial_failed_until_.end()) {
    if (backoff->second > now) {
      if (on_complete) {
        on_complete(Error("amp link: dial in backoff"));
      }
      return;
    }
    dial_failed_until_.erase(backoff);
  }

  if (concurrent_dials_ >= config_.max_concurrent_dials) {
    if (on_complete) {
      on_complete(Error("amp link: too many concurrent dials"));
    }
    return;
  }

  adp::OpenParams params;
  params.key = PreSessionPeerKey();
  params.mint_id = true;
  params.peer = ep_it->second.endpoint;
  auto opened = endpoint_.Open(params);
  if (!opened) {
    if (on_complete) {
      on_complete(opened.error());
    }
    return;
  }

  ++concurrent_dials_;
  inflight_associations_[peer_key].push_back(std::move(on_complete));

  auto link = std::make_unique<PeerLink>(peer_key, ep_it->second.peer_id, true, *opened, local_identity_, *this);
  link->StartOutboundHandshake([this, peer_key](Roe<void> result) {
    FinishDial(peer_key, result);
  });
  links_[peer_key] = std::move(link);
}

void PeerLinkManager::OpenChannelOnLink(PeerLink& link, const std::string& protocol_id, ChannelPolicy policy,
                                        ChannelCb on_complete) {
  if (link.Phase() != PeerLinkPhase::Connected || !link.Mux()) {
    if (on_complete) {
      on_complete(Error("amp link: association not ready"));
    }
    return;
  }
  auto channel_id = link.Mux()->OpenOutbound(protocol_id, policy);
  if (on_complete) {
    on_complete(channel_id);
  }
}

void PeerLinkManager::OpenChannel(const std::string& peer_key, const std::string& protocol_id, ChannelPolicy policy,
                                  ChannelCb on_complete) {
  EnsureAssociation(peer_key, [this, peer_key, protocol_id, policy = std::move(policy),
                                 on_complete = std::move(on_complete)](Roe<void> assoc) mutable {
    if (!assoc) {
      if (on_complete) {
        on_complete(assoc.error());
      }
      return;
    }
    auto* link = FindLink(peer_key);
    if (!link) {
      if (on_complete) {
        on_complete(Error("amp link: association not ready"));
      }
      return;
    }
    OpenChannelOnLink(*link, protocol_id, policy, std::move(on_complete));
  });
}

void PeerLinkManager::SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler) {
  protocol_handlers_[protocol_id] = std::move(handler);
  for (auto& [_, link] : links_) {
    ApplyProtocolHandlers(*link);
  }
}

void PeerLinkManager::RemoveProtocolHandler(const std::string& protocol_id) {
  protocol_handlers_.erase(protocol_id);
  for (auto& [_, link] : links_) {
    if (link->Mux()) {
      link->Mux()->SetProtocolHandler(protocol_id, {});
    }
  }
}

void PeerLinkManager::ClearProtocolHandlers() {
  protocol_handlers_.clear();
  for (auto& [_, link] : links_) {
    if (link->Mux()) {
      link->Mux()->ClearProtocolHandlers();
    }
  }
}

void PeerLinkManager::ApplyProtocolHandlers(PeerLink& link) {
  if (!link.Mux()) {
    return;
  }
  const std::string peer_key = link.PeerKey();
  link.Mux()->ClearProtocolHandlers();
  for (const auto& [protocol_id, handler] : protocol_handlers_) {
    link.Mux()->SetProtocolHandler(protocol_id, [this, peer_key, handler](const uint32_t channel_id,
                                                                           const std::string&) {
      if (!handler) {
        return;
      }
      if (auto* live = FindLink(peer_key)) {
        handler(*live, channel_id);
      }
    });
  }
}

void PeerLinkManager::FinishDial(const std::string& peer_key, Roe<void> result) {
  if (concurrent_dials_ > 0) {
    --concurrent_dials_;
  }
  if (!result) {
    last_error_[peer_key] = result.error().message;
    dial_failed_until_[peer_key] = std::chrono::steady_clock::now() + config_.dial_failure_backoff;
    links_.erase(peer_key);
  } else {
    last_error_.erase(peer_key);
  }

  auto waiters = std::move(inflight_associations_[peer_key]);
  inflight_associations_.erase(peer_key);
  for (auto& waiter : waiters) {
    if (waiter) {
      waiter(result);
    }
  }
}

void PeerLinkManager::OnInboundConnection(std::shared_ptr<adp::Connection> connection) {
  std::string peer_key = "inbound:";
  for (size_t i = 0; i < connection->Id().bytes.size(); ++i) {
    peer_key.push_back(static_cast<char>('0' + (connection->Id().bytes[i] >> 4)));
    peer_key.push_back(static_cast<char>('0' + (connection->Id().bytes[i] & 0x0f)));
  }
  if (links_.contains(peer_key)) {
    return;
  }
  auto link = std::make_unique<PeerLink>(peer_key, std::string{}, false, std::move(connection), local_identity_, *this);
  link->StartInboundHandshake({});
  links_[peer_key] = std::move(link);
}

void PeerLinkManager::RekeyLink(const std::string& from_key, const std::string& to_key) {
  if (from_key == to_key || links_.contains(to_key)) {
    return;
  }
  auto node = links_.extract(from_key);
  if (node.empty()) {
    return;
  }
  node.mapped()->SetPeerKey(to_key);
  if (!node.mapped()->RemotePeerId().empty()) {
    peer_id_to_key_[node.mapped()->RemotePeerId()] = to_key;
  }
  links_.emplace(to_key, std::move(node.mapped()));
}

void PeerLinkManager::TryAdoptInboundLink(PeerLink& inbound) {
  if (inbound.IsOutbound() || inbound.RemotePeerId().empty()) {
    return;
  }
  if (auto* existing = FindConnectedLinkForPeerId(inbound.RemotePeerId())) {
    if (existing != &inbound) {
      links_.erase(inbound.PeerKey());
      return;
    }
  }
  for (const auto& [alias, rec] : endpoints_) {
    if (rec.peer_id == inbound.RemotePeerId() && !links_.contains(alias)) {
      RekeyLink(inbound.PeerKey(), alias);
      return;
    }
  }
  peer_id_to_key_[inbound.RemotePeerId()] = inbound.PeerKey();
}

void PeerLinkManager::OnLinkEstablished(PeerLink& link) {
  if (!link.RemotePeerId().empty()) {
    peer_id_to_key_[link.RemotePeerId()] = link.PeerKey();
  }
  TryAdoptInboundLink(link);
  ApplyProtocolHandlers(link);
}

void PeerLinkManager::MarkWarm(const std::string& peer_key) {
  if (auto* link = FindLink(peer_key)) {
    link->MarkWarm();
  }
}

void PeerLinkManager::ClearWarm(const std::string& peer_key) {
  if (auto* link = FindLink(peer_key)) {
    link->ClearWarm();
  }
}

void PeerLinkManager::Tick() {
  const int64_t now = endpoint_.GetClock().NowMs();
  std::vector<std::string> evict;
  for (auto& [key, link] : links_) {
    if (!link->Connection().LooksAlive(now) && !link->IsWarm() && link->Phase() == PeerLinkPhase::Connected) {
      evict.push_back(key);
    }
  }
  for (const auto& key : evict) {
    links_.erase(key);
  }
}

} // namespace pbr::amp
