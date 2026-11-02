#pragma once

#include "lib/content/CasTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <optional>
#include <vector>

namespace pbr {

/**
 * The content store's index (CR024): one row per (realm, content id) with its metadata, pin and provenance. No
 * bytes (C002); `CasStore` keeps those. The app's storage keeps it on disk (`lib/storage/SqliteObjectIndex`), tests
 * in memory. Every method refuses a content id that is not `kCasContentIdSize` bytes. Safe from any thread.
 */
class IObjectIndex {
public:
  virtual ~IObjectIndex() = default;

  /** Insert or update a row. A new row with `created_at_ms` 0 is stamped now; an existing one keeps its time. */
  virtual Roe<void> Upsert(const CasObjectMeta& meta) = 0;
  virtual Roe<std::optional<CasObjectMeta>> Lookup(CasRealm realm, const ByteVector& content_id) const = 0;
  /** Removing a missing row is not an error. */
  virtual Roe<void> Remove(CasRealm realm, const ByteVector& content_id) = 0;
  /** Setting the pin of a missing row is not an error and adds none. */
  virtual Roe<void> SetPinned(CasRealm realm, const ByteVector& content_id, bool pinned) = 0;
  /**
   * The rows, limited to `realm` and `pinned` when given: newest first, then by content id (hex, ascending).
   */
  virtual Roe<std::vector<CasObjectMeta>> List(std::optional<CasRealm> realm = std::nullopt,
                                               std::optional<bool> pinned = std::nullopt) const = 0;
};

} // namespace pbr
