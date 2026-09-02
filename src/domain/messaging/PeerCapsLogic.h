#pragma once

#include "domain/messaging/CallTypes.h"
#include "common/directory/MeshHopTypes.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/**
 * SoftMigrate hop filter (V030): keep org seeds always; keep contacts only when known to
 * advertise media_relay. Unknown / missing ads → drop (fail closed). PreferLocal self is
 * prepended after this filter.
 */
std::vector<MeshHopCandidate> FilterHopsByMediaRelayAds(
    std::vector<MeshHopCandidate> ranked,
    const std::function<bool(const std::string& peer_id)>& has_media_relay_ad);

/**
 * Prepend PeerIds that advertised media_relay on call caps but may be missing from the
 * contact hop list (phone initiator SoftMigrate → desktop Node on the call).
 * `resolve_multiaddr` fills dial hints; empty multiaddr is ok if dial registry can resolve later.
 */
std::vector<MeshHopCandidate> MergeAdvertisedMediaRelayHops(
    std::vector<MeshHopCandidate> ranked, const std::vector<std::string>& advertised_media_relay_peer_ids,
    const std::function<std::string(const std::string& peer_id)>& resolve_multiaddr);

/** PeerIds from listen multiaddrs (`/p2p/<id>`), stable unique order. */
std::vector<std::string> PeerIdsFromListenMultiaddrs(const std::vector<std::string>& multiaddrs);

} // namespace pbr
