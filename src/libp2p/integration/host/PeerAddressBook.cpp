#include "libp2p/integration/host/PeerAddressBook.h"

#include "libp2p/integration/host/Libp2pHost.h"

#include <libp2p/host/host.hpp>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/peer/peer_id.hpp>

#include <algorithm>

namespace pbr {
namespace {

std::string PeerIdFromMultiaddrString(const std::string& multiaddr) {
  const std::string marker = "/p2p/";
  const auto pos = multiaddr.rfind(marker);
  if (pos == std::string::npos) {
    return {};
  }
  std::string id = multiaddr.substr(pos + marker.size());
  const auto slash = id.find('/');
  if (slash != std::string::npos) {
    id.resize(slash);
  }
  return id;
}

std::string MultiaddrWithPeerId(const libp2p::multi::Multiaddress& address,
                                const std::string& peer_id_base58) {
  if (const auto embedded = address.getPeerId(); embedded && *embedded == peer_id_base58) {
    return address.getStringAddress();
  }
  auto with_peer = libp2p::multi::Multiaddress::create(address.getStringAddress() + "/p2p/" +
                                                         peer_id_base58);
  if (with_peer) {
    return with_peer->getStringAddress();
  }
  return address.getStringAddress();
}

int SourceRank(PeerAddrSource source) {
  switch (source) {
  case PeerAddrSource::DialSuccess:
    return 6;
  case PeerAddrSource::Connection:
    return 5;
  case PeerAddrSource::Identify:
    return 5;
  case PeerAddrSource::AddressRepository:
    return 4;
  case PeerAddrSource::Bootstrap:
    return 3;
  case PeerAddrSource::Contact:
    return 2;
  case PeerAddrSource::Manual:
    return 1;
  }
  return 0;
}

} // namespace

PeerAddressBook::PeerAddressBook(PeerAddressBookConfig config) : config_(std::move(config)) {}

void PeerAddressBook::SetConfig(PeerAddressBookConfig config) {
  std::lock_guard lock(mutex_);
  config_ = std::move(config);
}

Roe<void> PeerAddressBook::Upsert(const std::string& peer_id_base58, const std::string& multiaddr,
                                  PeerAddrSource source) {
  if (peer_id_base58.empty() || multiaddr.empty()) {
    return Error("empty peer address book entry");
  }
  auto parsed = libp2p::multi::Multiaddress::create(multiaddr);
  if (!parsed) {
    return Error("invalid multiaddr");
  }
  const std::string embedded = PeerIdFromMultiaddrString(multiaddr);
  if (!embedded.empty() && embedded != peer_id_base58) {
    return Error("multiaddr PeerId mismatch");
  }
  auto peer_id = libp2p::peer::PeerId::fromBase58(peer_id_base58);
  if (!peer_id) {
    return Error("invalid PeerId");
  }
  libp2p::peer::PeerInfo info{peer_id.value(), {parsed.value()}};
  UpsertParsedLocked(peer_id_base58, std::move(info), source);
  return {};
}

void PeerAddressBook::IngestPeerInfo(const libp2p::peer::PeerInfo& info, PeerAddrSource source) {
  if (info.addresses.empty()) {
    return;
  }
  const std::string peer_id_base58 = info.id.toBase58();
  UpsertParsedLocked(peer_id_base58, info, source);
}

bool PeerAddressBook::IsDialable(const std::string& peer_id_base58) const {
  std::lock_guard lock(mutex_);
  const auto it = peers_.find(peer_id_base58);
  if (it == peers_.end()) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  for (const AddrEntry& entry : it->second.addrs) {
    if (entry.expires_at > now && !entry.multiaddr.empty()) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> PeerAddressBook::PreferredMultiaddr(
    const std::string& peer_id_base58) const {
  std::lock_guard lock(mutex_);
  const auto it = peers_.find(peer_id_base58);
  if (it == peers_.end()) {
    return std::nullopt;
  }
  const auto now = std::chrono::steady_clock::now();
  const AddrEntry* best = nullptr;
  for (const AddrEntry& entry : it->second.addrs) {
    if (entry.expires_at <= now || entry.multiaddr.empty()) {
      continue;
    }
    if (!best || SourceRank(entry.source) > SourceRank(best->source) ||
        (SourceRank(entry.source) == SourceRank(best->source) &&
         entry.expires_at > best->expires_at)) {
      best = &entry;
    }
  }
  if (!best) {
    return std::nullopt;
  }
  return best->multiaddr;
}

std::optional<libp2p::peer::PeerInfo> PeerAddressBook::ResolvePeerInfo(
    const std::string& peer_id_base58) const {
  auto ma = PreferredMultiaddr(peer_id_base58);
  if (!ma) {
    return std::nullopt;
  }
  auto peer_id = libp2p::peer::PeerId::fromBase58(peer_id_base58);
  if (!peer_id) {
    return std::nullopt;
  }
  auto parsed = libp2p::multi::Multiaddress::create(*ma);
  if (!parsed) {
    return std::nullopt;
  }
  libp2p::peer::PeerInfo info{peer_id.value(), {parsed.value()}};
  return info;
}

void PeerAddressBook::SyncFromHost(Libp2pHost& host, const std::string& peer_id_base58) {
  if (!host.IsRunning() || peer_id_base58.empty()) {
    return;
  }
  auto peer_id = libp2p::peer::PeerId::fromBase58(peer_id_base58);
  if (!peer_id) {
    return;
  }
  auto addresses =
      host.GetHost().getPeerRepository().getAddressRepository().getAddresses(peer_id.value());
  if (!addresses) {
    return;
  }
  libp2p::peer::PeerInfo info{peer_id.value(), addresses.value()};
  IngestPeerInfo(info, PeerAddrSource::AddressRepository);
}

void PeerAddressBook::PruneExpired() {
  std::lock_guard lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = peers_.begin(); it != peers_.end();) {
    auto& addrs = it->second.addrs;
    addrs.erase(std::remove_if(addrs.begin(), addrs.end(),
                               [&](const AddrEntry& entry) {
                                 return entry.expires_at <= now || entry.multiaddr.empty();
                               }),
                addrs.end());
    if (addrs.empty()) {
      it = peers_.erase(it);
    } else {
      ++it;
    }
  }
}

size_t PeerAddressBook::PeerCount() const {
  std::lock_guard lock(mutex_);
  return peers_.size();
}

std::chrono::milliseconds PeerAddressBook::TtlFor(PeerAddrSource source) const {
  switch (source) {
  case PeerAddrSource::DialSuccess:
    return config_.dial_success_ttl;
  case PeerAddrSource::Connection:
    return config_.connection_ttl;
  default:
    return config_.default_ttl;
  }
}

std::chrono::steady_clock::time_point PeerAddressBook::ExpiresAt(PeerAddrSource source) const {
  return std::chrono::steady_clock::now() + TtlFor(source);
}

void PeerAddressBook::UpsertParsedLocked(const std::string& peer_id_base58,
                                           libp2p::peer::PeerInfo info, PeerAddrSource source) {
  const auto expires_at = ExpiresAt(source);
  PeerEntry& peer = peers_[peer_id_base58];
  for (const libp2p::multi::Multiaddress& address : info.addresses) {
    const std::string ma = MultiaddrWithPeerId(address, peer_id_base58);
    auto existing = std::find_if(peer.addrs.begin(), peer.addrs.end(),
                                 [&](const AddrEntry& entry) { return entry.multiaddr == ma; });
    if (existing == peer.addrs.end()) {
      peer.addrs.push_back(AddrEntry{ma, expires_at, source});
      continue;
    }
    if (expires_at > existing->expires_at) {
      existing->expires_at = expires_at;
    }
    if (SourceRank(source) >= SourceRank(existing->source)) {
      existing->source = source;
    }
  }
}

} // namespace pbr
