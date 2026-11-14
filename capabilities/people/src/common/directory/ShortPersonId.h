#pragma once

#include <string>

namespace pbr {

/** Shorten a person's id (an Account ID, a PeerId) for UI to its first characters: people's labels and notices. */
inline std::string ShortPersonId(const std::string& account_or_peer_id) {
  constexpr size_t kKeep = 12;
  if (account_or_peer_id.size() <= kKeep + 1) {
    return account_or_peer_id;
  }
  return account_or_peer_id.substr(0, kKeep) + "…";
}

} // namespace pbr
