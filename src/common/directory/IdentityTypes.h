#pragma once

#include <cstdint>
#include <string>

namespace pbr {

struct LocalIdentity {
  /**
   * Device ML-DSA-65 public key (base64). Derives Peer ID / libp2p Noise identity (P004).
   * Not the person/account signing key (account_signing_*).
   */
  std::string public_key_b64;
  /** Device ML-DSA-65 private key (base64). Plaintext only in memory; on disk inside identity.enc. */
  std::string private_key_b64;
  /** Account ML-DSA-65 public key (base64). Empty until minted (m1). */
  std::string account_signing_public_key_b64;
  /** Account ML-DSA-65 private key (base64). Under DEK with the rest of identity.enc. */
  std::string account_signing_private_key_b64;
  /**
   * Portable person id: account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk))> (M002).
   * Empty until account key material exists.
   */
  std::string account_id;
  /** Account ML-KEM-768 public key (base64). Directory encapsulate-to for public/group auto-key (M015). */
  std::string kem_public_key_b64;
  /** Account ML-KEM-768 private key (base64). Copied on link-device; not the device key. */
  std::string kem_private_key_b64;
  /**
   * This device's own ML-KEM-768 key pair (base64; M021): peers open sessions with this device under it.
   * Never leaves the device (not in a link bundle, nor in the account locker); minted on first load.
   */
  std::string device_kem_public_key_b64;
  std::string device_kem_private_key_b64;
  /**
   * The 32-byte recovery seed the account keys derive from (base64; multi-device M023 §2) — the recovery phrase's
   * content. Empty for an account minted before M023 (random account keys: no phrase) or a pp-node (its own seed).
   * Under DEK with the rest of identity.enc; travels with the account keys in a link bundle.
   */
  std::string recovery_seed_b64;
  std::string nickname;
  /** Device endpoint: mesh PeerId base58; derived in memory from device ML-DSA pubkey. */
  std::string peer_id;
  /** Transport handle (route): relay-assigned; empty until registered (D082 / M006). */
  std::string relay_user_id;
  /** Brief LLM API key (plaintext in memory; persisted inside identity.enc). Empty until registered. */
  std::string brief_llm_api_key;
  /**
   * Free-tier guest Brief Bearer (`brf_guest_*`) minted without registration.
   * Ignored when brief_llm_api_key is set. Cleared after successful register/finish.
   */
  std::string brief_llm_guest_api_key;
  bool registered = false;
  /** ISO-8601 expiry from register/finish; empty until registered or if server omitted it. */
  std::string registration_expires_at;
  /**
   * Own initiation floor (pp_credit minor units, P001). Default 0.
   * Dogfood via AppConfig.initiation_floor; published on register/renew when server supports it.
   */
  int64_t initiation_floor = 0;
  /**
   * The relay's directory entry carries this account's KEM key binding (foundation/crypto/KemKeyBinding.h):
   * peers take the KEM key only with it. A registration from before bindings re-registers once.
   */
  bool kem_binding_published = false;
};

} // namespace pbr
