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
 */
CallHopScope InferCallHopScope(
    const std::vector<std::string>& local_mas,
    const std::unordered_map<std::string, std::vector<std::string>>& remote_mas_by_peer);

/**
 * Scope-aware SoftMigrate hop order (V035).
 * Link/Site + prefer_local_as_hop + local_advertise_ma → PreferLocal first.
 * Wide → never PreferLocal with private advertise MA; promote dialable org/directory
 * public MAs. PreferLocal on Wide only when local_advertise_ma is non-private.
 */
std::vector<MeshHopCandidate> SelectCallMediaHop(std::vector<MeshHopCandidate> ranked,
                                                 CallHopScope scope,
                                                 const std::string& local_peer_id,
                                                 bool prefer_local_as_hop,
                                                 const std::string& local_advertise_ma);

/** True when PreferLocal is eligible for this scope + advertise MA. */
bool PreferLocalAllowedForScope(CallHopScope scope, bool prefer_local_as_hop,
                                const std::string& local_advertise_ma);

} // namespace pbr
