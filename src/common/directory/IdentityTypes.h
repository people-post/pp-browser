#pragma once

#include "common/Secret.h"

#include <cstdint>
#include <string>

namespace pbr {

/** LocalIdentity::card_intent values (M021 d4). */
inline constexpr const char kCardIntentFresh[] = "fresh";
inline constexpr const char kCardIntentJoin[] = "join";

/** Private keys, the recovery seed and API keys are SecretText (common/Secret.h): they cannot be logged. */
struct LocalIdentity {
  /**
   * Device ML-DSA-65 public key (base64). Derives Peer ID / libp2p Noise identity (P004).
   * Not the person/account signing key (account_signing_*).
   */
  std::string public_key_b64;
  /** Device ML-DSA-65 private key (base64). Plaintext only in memory; on disk inside identity.enc. */
  SecretText private_key_b64;
  /** Account ML-DSA-65 public key (base64). Empty until minted (m1). */
  std::string account_signing_public_key_b64;
  /** Account ML-DSA-65 private key (base64). Under DEK with the rest of identity.enc. */
  SecretText account_signing_private_key_b64;
  /**
   * Portable person id: account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk))> (M002).
   * Empty until account key material exists.
   */
  std::string account_id;
  /** Account ML-KEM-768 public key (base64). Directory encapsulate-to for public/group auto-key (M015). */
  std::string kem_public_key_b64;
  /** Account ML-KEM-768 private key (base64). Copied on link-device; not the device key. */
  SecretText kem_private_key_b64;
  /**
   * This device's own ML-KEM-768 key pair (base64; M021): peers open sessions with this device under it.
   * Never leaves the device (not in a link bundle, nor in the account locker); minted on first load.
   */
  std::string device_kem_public_key_b64;
  SecretText device_kem_private_key_b64;
  /**
   * The 32-byte recovery seed the account keys derive from (base64; multi-device M023 §2) — the recovery phrase's
   * content. Empty for an account minted before M023 (random account keys: no phrase) or a pp-node (its own seed).
   * Under DEK with the rest of identity.enc; travels with the account keys in a link bundle.
   */
  SecretText recovery_seed_b64;
  /**
   * How this device enters its account's device list (M021 d4) on its next card: `fresh` — a list holding only
   * itself, as the account's master (after a restore from the recovery phrase); `join` — appended to the newest
   * list (after a link; the master's transfer code is its consent). Empty once done, and for a device already on
   * the list.
   */
  std::string card_intent;
  std::string nickname;
  /** Device endpoint: mesh PeerId base58; derived in memory from device ML-DSA pubkey. */
  std::string peer_id;
  /** Transport handle (route): relay-assigned; empty until registered (D082 / M006). */
  std::string relay_user_id;
  /** Brief LLM API key (plaintext in memory; persisted inside identity.enc). Empty until registered. */
  SecretText brief_llm_api_key;
  /**
   * Free-tier guest Brief Bearer (`brf_guest_*`) minted without registration.
   * Ignored when brief_llm_api_key is set. Cleared after successful register/finish.
   */
  SecretText brief_llm_guest_api_key;
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
