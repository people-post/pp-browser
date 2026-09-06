#pragma once

#include "domain/messaging/CasTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace pbr {

/**
 * SQLite index for CAS objects (`{profile}/object_index.db`).
 * Does not store bytes — only metadata / pins / provenance (C002).
 */
class ObjectIndex {
public:
  explicit ObjectIndex(std::string profile_dir);
  ~ObjectIndex();

  ObjectIndex(const ObjectIndex&) = delete;
  ObjectIndex& operator=(const ObjectIndex&) = delete;

  Roe<void> Upsert(const CasObjectMeta& meta);
  Roe<std::optional<CasObjectMeta>> Lookup(CasRealm realm, const ByteVector& content_id) const;
  Roe<void> Remove(CasRealm realm, const ByteVector& content_id);
  Roe<void> SetPinned(CasRealm realm, const ByteVector& content_id, bool pinned);

  /**
   * List index rows. Pass nullopt realm/pinned to leave that axis unconstrained.
   * Ordered by created_at_ms DESC, then content_id_hex ASC.
   */
  Roe<std::vector<CasObjectMeta>> List(std::optional<CasRealm> realm = std::nullopt,
                                       std::optional<bool> pinned = std::nullopt) const;

  const std::string& DbPath() const { return db_path_; }

private:
  Roe<void> EnsureOpen() const;
  Roe<void> MigrateSchema(sqlite3* db) const;

  std::string profile_dir_;
  std::string db_path_;
  mutable sqlite3* db_ = nullptr;
  mutable std::mutex mutex_;
};

} // namespace pbr
