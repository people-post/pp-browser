#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pbr {

inline constexpr size_t kCasContentIdSize = 32;

/** Confidentiality realm for profile CAS (C001–C005). */
enum class CasRealm : uint8_t { Private = 0, Public = 1 };

inline const char* CasRealmToString(const CasRealm realm) {
  switch (realm) {
  case CasRealm::Private:
    return "private";
  case CasRealm::Public:
    return "public";
  }
  return "private";
}

inline std::optional<CasRealm> CasRealmFromString(const std::string_view value) {
  if (value == "private") {
    return CasRealm::Private;
  }
  if (value == "public") {
    return CasRealm::Public;
  }
  return std::nullopt;
}

struct CasObjectMeta {
  CasRealm realm = CasRealm::Private;
  ByteVector content_id;
  std::string mime;
  std::string filename;
  uint64_t byte_length = 0;
  /** Hex of private content_id when this public object was published from private (C002). */
  std::string published_from_hex;
  bool pinned = true;
  int64_t created_at_ms = 0;
};

} // namespace pbr
