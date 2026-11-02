#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include <string>

namespace pbr {

/**
 * A session with a peer (E008), named by its communicating identity and channel. With `peer_device` (the peer
 * device's PeerId) it names a session with one of the peer's devices (M021: an `e2e_public` session is per device
 * pair — this device is always the other end); without, the account-level session (private `e2e`, and the account
 * copy a first contact goes as).
 */
struct PeerSessionKey {
  std::string peer_identity_kind;
  std::string peer_identity_value;
  CryptoChannel channel = CryptoChannel::E2e;
  std::string peer_device;

  bool operator==(const PeerSessionKey& other) const {
    return peer_identity_kind == other.peer_identity_kind &&
           peer_identity_value == other.peer_identity_value && channel == other.channel &&
           peer_device == other.peer_device;
  }
};

} // namespace pbr
