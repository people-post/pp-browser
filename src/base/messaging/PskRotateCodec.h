#pragma once

#include "base/crypto/CryptoTypes.h"
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
