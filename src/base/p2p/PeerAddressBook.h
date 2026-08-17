#pragma once

#include "common/Error.h"

#include <libp2p/peer/peer_info.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

class Libp2pHost;

/** Provenance for TTL / debugging (media-hop L1). */
enum class PeerAddrSource {
  Bootstrap,
  Contact,
  Connection,
  DialSuccess,
  Identify,
  Mdns,
  /** CallSfuAttach / SoftMigrate hop — must outrank mDNS (virbr poison dogfood). */
  CallHop,
  AddressRepository,
  Manual,
};

struct PeerAddressBookConfig {
  std::chrono::milliseconds default_ttl{std::chrono::hours(24)};
  std::chrono::milliseconds connection_ttl{std::chrono::hours(24)};
  std::chrono::milliseconds dial_success_ttl{std::chrono::hours(48)};
};

/**
 * Integration-layer peer address book (media-hop L1).
 * Remembers dialable multiaddrs per PeerId (base58) with TTL; complements libp2p
 * InmemAddressRepository and string-keyed PeerSessionManager endpoints.
 */
class PeerAddressBook {
public:
  explicit PeerAddressBook(PeerAddressBookConfig config = {});

  void SetConfig(PeerAddressBookConfig config);

  Roe<void> Upsert(const std::string& peer_id_base58, const std::string& multiaddr, PeerAddrSource source);

  void IngestPeerInfo(const libp2p::peer::PeerInfo& info, PeerAddrSource source);

  bool IsDialable(const std::string& peer_id_base58) const;
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id_base58) const;
  std::optional<libp2p::peer::PeerInfo> ResolvePeerInfo(const std::string& peer_id_base58) const;

  /** Merge addresses from libp2p peer repository (non-expired) into the book. */
  void SyncFromHost(Libp2pHost& host, const std::string& peer_id_base58);

  /** Drop expired entries. Pass `now` from tests to avoid wall-clock sleeps. */
  void PruneExpired(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  size_t PeerCount() const;

private:
  std::chrono::milliseconds TtlFor(PeerAddrSource source) const;
  std::chrono::steady_clock::time_point ExpiresAt(PeerAddrSource source) const;

  struct AddrEntry {
    std::string multiaddr;
    std::chrono::steady_clock::time_point expires_at{};
    PeerAddrSource source = PeerAddrSource::Manual;
  };

  struct PeerEntry {
    std::vector<AddrEntry> addrs;
  };

  void UpsertParsedLocked(const std::string& peer_id_base58, libp2p::peer::PeerInfo info,
                          PeerAddrSource source);

  mutable std::mutex mutex_;
  PeerAddressBookConfig config_;
  std::unordered_map<std::string, PeerEntry> peers_;
};

} // namespace pbr
