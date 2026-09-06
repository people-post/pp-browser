#pragma once

#include "domain/messaging/CasTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/** UI/filter key for library listing (C013). */
enum class CasLibraryFilter : uint8_t { All = 0, Private = 1, Public = 2, Cache = 3 };

struct CasLibraryRow {
  std::string content_id_hex;
  CasRealm realm = CasRealm::Private;
  std::string title;
  std::string detail;       // size · mime
  std::string realm_label;  // Private / Public
  std::string pin_label;    // Kept / Cache
  bool pinned = true;
  bool can_share_publicly = false;
  bool can_unpublish = false;
  bool can_copy_tip = false;
};

inline const char* CasLibraryFilterToString(const CasLibraryFilter filter) {
  switch (filter) {
  case CasLibraryFilter::All:
    return "all";
  case CasLibraryFilter::Private:
    return "private";
  case CasLibraryFilter::Public:
    return "public";
  case CasLibraryFilter::Cache:
    return "cache";
  }
  return "all";
}

inline std::optional<CasLibraryFilter> CasLibraryFilterFromString(const std::string_view value) {
  if (value == "all") {
    return CasLibraryFilter::All;
  }
  if (value == "private") {
    return CasLibraryFilter::Private;
  }
  if (value == "public") {
    return CasLibraryFilter::Public;
  }
  if (value == "cache") {
    return CasLibraryFilter::Cache;
  }
  return std::nullopt;
}

/** Build library rows from object_index (no DEK required). */
Roe<std::vector<CasLibraryRow>> ListCasLibrary(const std::string& profile_dir, CasLibraryFilter filter);

/** Share publicly… / Unpublish… helpers used by Me → Storage. */
Roe<ByteVector> ShareCasPublicly(const std::string& profile_dir, const std::string& profile_id,
                                 const ByteVector& dek, const ByteVector& private_content_id);
Roe<void> UnpublishCasPublic(const std::string& profile_dir, const std::string& profile_id,
                             const ByteVector& public_content_id);

/** Link/tip share (U9): `pp-cas:v1:<64-hex>` or raw 64-hex content id. */
inline constexpr const char* kCasPublicTipPrefix = "pp-cas:v1:";
std::string FormatCasPublicTip(const std::string& content_id_hex);
Roe<ByteVector> ParseCasPublicTip(std::string_view tip);

/** Serve gate (P4): pinned public only — never private, never Cache. */
Roe<ByteVector> LoadPinnedPublicCasBytes(const std::string& profile_dir, const std::string& profile_id,
                                       const ByteVector& content_id);

/** Peer-fetched public object → public Cache (C013). */
Roe<void> CacheFetchedPublicCas(const std::string& profile_dir, const std::string& profile_id,
                              const ByteVector& content_id, const ByteVector& bytes,
                              std::string_view mime = {}, std::string_view filename = {});

} // namespace pbr
