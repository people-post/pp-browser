#pragma once

#include <string>

namespace pbr {

struct LocalIdentity {
  std::string public_key_b64;
  std::string encrypted_private_key_b64;
  std::string kem_public_key_b64;
  std::string kem_private_key_b64;
  std::string nickname;
  /** Network identity (who): libp2p PeerId base58; derived in memory from signing pubkey (D096). */
  std::string peer_id;
  /** Transport handle (route): relay-assigned; empty until registered (D082 / D096). */
  std::string relay_user_id;
  bool registered = false;
};

} // namespace pbr
