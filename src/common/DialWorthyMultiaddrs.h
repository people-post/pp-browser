#pragma once

#include "mesh/types/MeshHopDial.h"

#include <algorithm>
#include <string>
#include <vector>
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The addresses worth publishing or dialling from off the node's own network: private-range and
 * local-scope IP addresses (a container's, a LAN's) are dropped when any other address is listed —
 * a dial that falls back to one only times out (#379). A list of nothing else is kept as it is.
 */
inline std::vector<std::string> DialWorthyMultiaddrs(std::vector<std::string> multiaddrs) {
  const auto local_scope = [](const std::string& ma) {
    const bool ip = ma.rfind("/ip6/", 0) == 0 || ma.find("/ip4/") != std::string::npos;
    return ip && !mesh::MultiaddrHasPublicDialHost(ma);
  };
  if (!std::all_of(multiaddrs.begin(), multiaddrs.end(), local_scope)) {
    std::erase_if(multiaddrs, local_scope);
  }
  return multiaddrs;
}

} // namespace pbr
