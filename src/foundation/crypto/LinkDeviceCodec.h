#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * QR / paste payload `pp-browser-link-device-v1` (M012): the account keys, the shared DEK, the relay binding,
 * the nickname, the issuer's account card and the recovery seed. No sessions: each device opens its own, per
 * device pair (M021). Keys the parser does not know (an older build's `public_psks`) are ignored.
 */
struct LinkDeviceBundleV1 {
  std::string account_id;
  std::string account_ml_dsa_pk_b64;
  std::string account_ml_dsa_sk_b64;
  /** Account ML-KEM-768 — public auto-key / group pair encapsulate-to (M015). */
  std::string account_kem_pk_b64;
  std::string account_kem_sk_b64;
  std::string dek_b64;
  std::string relay_user_id;
  std::string nickname;
  int64_t created_at_ms = 0;
  int64_t expires_at_ms = 0;
  /**
   * The issuing device's account card (JSON text; M021): the account's device list, which the new device
   * signs itself into. Absent in a recovery bundle (a restore starts a list of its own).
   */
  std::optional<std::string> account_card_json;
  /**
   * The recovery seed the account keys derive from (base64; multi-device M023 §2), when the account has one: the
   * new device can show the same phrase. Checked on Validate — it must derive exactly the bundle's account keys.
   */
  std::optional<std::string> recovery_seed_b64;
};

class LinkDeviceCodec {
public:
  static Roe<std::string> Serialize(const LinkDeviceBundleV1& bundle);
  static Roe<LinkDeviceBundleV1> Parse(const std::string& json);
  static Roe<void> Validate(const LinkDeviceBundleV1& bundle, int64_t now_ms = 0);
};

} // namespace pbr
