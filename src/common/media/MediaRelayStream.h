#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/**
 * The `media_relay` stream a publisher sends on, derived from an identity every subscriber knows
 * (FNV-1a; never 0): calls use the Account id, broadcast the publisher's mesh PeerId.
 */
inline uint32_t PublisherStreamIdForIdentity(const std::string& identity) {
  if (identity.empty()) {
    return 1u;
  }
  uint32_t h = 2166136261u;
  for (unsigned char c : identity) {
    h ^= c;
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

} // namespace pbr
