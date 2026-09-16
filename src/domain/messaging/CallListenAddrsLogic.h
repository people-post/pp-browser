#pragma once

#include "domain/messaging/PeerCapsLogic.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * Fill call-control listen fields from local Amp advertise (invite/accept).
 * mDNS is optional — non-empty local_mas alone must dial the peer (V038 D3).
 */
inline void FillCallListenFields(const std::vector<std::string>& local_mas,
                                 std::string& libp2p_peer_id,
                                 std::vector<std::string>& listen_multiaddrs) {
  listen_multiaddrs = local_mas;
  if (libp2p_peer_id.empty()) {
    const auto ids = PeerIdsFromListenMultiaddrs(local_mas);
    if (!ids.empty()) {
      libp2p_peer_id = ids.front();
    }
  }
}

/**
 * True when invite/accept listen MAs are enough to attempt dial without mDNS peerstore.
 * Empty → caller must rely on other discovery (mDNS / punch / circuit).
 */
inline bool InviteListenAddrsSufficientForDirectDial(const std::vector<std::string>& listen_multiaddrs) {
  return !listen_multiaddrs.empty() && !PeerIdsFromListenMultiaddrs(listen_multiaddrs).empty();
}

} // namespace pbr
