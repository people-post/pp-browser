#pragma once

#include <cstdint>
#include <string>

namespace pbr {

struct LocalIdentity {
  std::string public_key_b64;
  /** Ed25519 private key (base64). Plaintext only in memory; on disk inside identity.enc. */
  std::string private_key_b64;
  std::string kem_public_key_b64;
  std::string kem_private_key_b64;
  std::string nickname;
  /** Network identity (who): libp2p PeerId base58; derived in memory from signing pubkey (D096). */
  std::string peer_id;
  /** Transport handle (route): relay-assigned; empty until registered (D082 / D096). */
  std::string relay_user_id;
  /** Brief LLM API key (plaintext in memory; persisted inside identity.enc). Empty until registered. */
  std::string brief_llm_api_key;
  bool registered = false;
  /** ISO-8601 expiry from register/finish; empty until registered or if server omitted it. */
  std::string registration_expires_at;
  /**
   * Own initiation floor (pp_credit minor units, P001). Default 0.
   * Dogfood via AppConfig.initiation_floor; published on register/renew when server supports it.
   */
  int64_t initiation_floor = 0;
};

} // namespace pbr
