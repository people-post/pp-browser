#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/thread/ThreadTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct PskRotateDetail {
  std::string rotation_id;
  uint32_t new_epoch = 0;
  std::string wrap_kind;
  std::string thread_kem_pk_b64;
  std::string key_init_hash;
};

/**
 * The stored text of the rotate line the sender keeps in its own transcript: a marker, not words (the UI words
 * it in the reader's language). Rows stored before held the English "New messages stay on this device." and
 * "Chat key updated."; the UI reads those too (LegacySystemLineText).
 */
inline constexpr const char* kPskRotateLockText = "psk_rotate:lock";
inline constexpr const char* kPskRotateRekeyText = "psk_rotate:rekey";

class PskRotateCodec {
public:
  static bool IsPskRotateMessage(const ThreadMessage& message);
  static Roe<PskRotateDetail> Decode(const ThreadMessage& message);
  static Roe<std::string> EncodePayloadJson(const PskRotateDetail& detail);
  static Roe<void> Validate(const PskRotateDetail& detail);
  /** True if `left` wins a concurrent rotation (lexicographically greater `rotation_id`). */
  static bool RotationIdWins(const std::string& left, const std::string& right);
};

} // namespace pbr
