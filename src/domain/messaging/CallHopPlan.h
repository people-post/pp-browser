#pragma once

#include "common/directory/MeshHopTypes.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

/** Inferred SoftMigrate hop band from listen multiaddrs (V035 / N023). */
enum class CallHopScope {
  Link = 0,
  Site = 1,
  Wide = 2,
};

/**
 * Infer call hop scope from local advertise MAs vs each remote peer's listen MAs.
 * Empty remotes map → Wide (fail closed). Any remote with empty listen list → Wide.
 *
 * Note: same RFC1918 /24 across remotes is only a *candidate* Link — PreferLocal still
 * requires lan_reachability_confirmed (coincidental 192.168.1.0/24 on different LANs).
 */
CallHopScope InferCallHopScope(
    const std::vector<std::string>& local_mas,
    const std::unordered_map<std::string, std::vector<std::string>>& remote_mas_by_peer);

/**
 * Scope-aware SoftMigrate hop order (V035).
 * PreferLocal only for confirmed Link (not Site; not unconfirmed same-/24).
 * Wide / Site → org/directory public MAs first. PreferLocal on Wide only when
 * local_advertise_ma is non-private.
 */
std::vector<MeshHopCandidate> SelectCallMediaHop(std::vector<MeshHopCandidate> ranked,
                                                 CallHopScope scope,
                                                 const std::string& local_peer_id,
                                                 bool prefer_local_as_hop,
                                                 const std::string& local_advertise_ma,
                                                 bool lan_reachability_confirmed = false);

/**
 * PreferLocal eligibility (V035).
 * - Link + private advertise → only when lan_reachability_confirmed
 * - Site → never PreferLocal (different private subnets cannot dial PreferLocal MA)
 * - Wide → only when advertise MA is non-private
 */
bool PreferLocalAllowedForScope(CallHopScope scope, bool prefer_local_as_hop,
                                const std::string& local_advertise_ma,
                                bool lan_reachability_confirmed = false);

/** True when local advertise includes a publicly dialable IPv4 or global IPv6 host. */
bool LocalAdvertiseHasPublicIpv4(const std::vector<std::string>& local_mas);

/**
 * Guest: may dial a private hop MA only when same /24 as local private advertise
 * and local is not also advertising a public IPv4 (WAN node must not dial PreferLocal LAN).
 */
bool GuestMayDialPrivateHopMa(const std::string& hop_multiaddr,
                              const std::vector<std::string>& local_mas);

} // namespace pbr
