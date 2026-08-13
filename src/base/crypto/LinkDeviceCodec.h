#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/IPskSessionStore.h"

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Syncable PSK row for link-device — `e2e_public` (and later group) only (M005). */
struct LinkDevicePublicPsk {
  ChatTargetKey key;
  uint32_t session_epoch = 1;
  std::string master_psk_b64;
  std::optional<int64_t> psk_verified_at;
  std::vector<RetiredPskEntry> retired_psks;
};

/**
 * QR / paste payload `pp-browser-link-device-v1` (M012).
 * Never includes private (`e2e`) PSKs.
 */
struct LinkDeviceBundleV1 {
  std::string account_id;
  std::string account_ml_dsa_pk_b64;
  std::string account_ml_dsa_sk_b64;
  std::string dek_b64;
  std::string relay_user_id;
  std::string nickname;
  int64_t created_at_ms = 0;
  int64_t expires_at_ms = 0;
  std::vector<LinkDevicePublicPsk> public_psks;
};

class LinkDeviceCodec {
public:
  static Roe<std::string> Serialize(const LinkDeviceBundleV1& bundle);
  static Roe<LinkDeviceBundleV1> Parse(const std::string& json);
  static Roe<void> Validate(const LinkDeviceBundleV1& bundle, int64_t now_ms = 0);
};

} // namespace pbr
