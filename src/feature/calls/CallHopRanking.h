#pragma once

#include "domain/messaging/CallHopPlan.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/MeshHopPolicy.h"
#include "feature/calls/CallTopologyRelayDeps.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The media hops this device could use, ranked (contacts ∪ directory ∪ DHT ∪ seeds, escalating, V030
 * media_relay ads, dialable), and what the ranking needs about the call's peers: the hop scope their
 * listen addrs imply (V035) and whether one is LAN-confirmed. Reads the topology's relay deps
 * (non-owning; rebound on SetMediaRelayDeps). Calls owner.
 */
class CallHopRanking {
public:
  explicit CallHopRanking(ContactsStore& contacts) : contacts_(contacts) {}

  void SetMediaRelayDeps(const CallTopologyMediaRelayDeps* deps) { deps_ = deps; }

  std::vector<MeshHopCandidate> Ranked() const;
  /** A hop we could move to now (or we host one). */
  bool HasCandidates() const;
  /** A dialable multiaddr for `hop_peer_id` (ranked candidates, then the dial registry); empty if none. */
  std::string HopMultiaddr(const std::string& hop_peer_id) const;

  /** Our advertised listen addrs (live, else configured, else the listen addr). */
  std::vector<std::string> LocalAdvertiseMas() const;
  /** The first of them, with `/p2p/<local_peer_id>` when it lacks one. */
  std::string LocalAdvertiseMa(const std::string& local_peer_id) const;
  /** The hop scope these remotes' listen addrs imply (unknown → Wide). */
  CallHopScope ScopeForPeers(const std::vector<std::string>& remote_identities) const;
  /** One of these remotes (or a PeerId in its listen addrs) is LAN-confirmed. */
  bool LanConfirmedForPeers(const std::vector<std::string>& remote_identities) const;
  /** Our mesh PeerId from the relay client; empty without one. */
  std::string LocalPeerId() const;

private:
  void FillDialInfo(std::vector<MeshHopCandidate>& ranked, bool published_ma_is_dialable) const;

  ContactsStore& contacts_;
  const CallTopologyMediaRelayDeps* deps_ = nullptr;
};

} // namespace pbr
