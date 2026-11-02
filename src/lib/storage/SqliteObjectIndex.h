#pragma once

#include "lib/content/IObjectIndex.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace pbr {

/**
 * The content index on disk (CR024): `{profile}/object_index.db`, its own file beside the profile database. Opened
 * on first use and held open.
 */
class SqliteObjectIndex : public IObjectIndex {
public:
  explicit SqliteObjectIndex(std::string profile_dir);
  ~SqliteObjectIndex() override;

  SqliteObjectIndex(const SqliteObjectIndex&) = delete;
  SqliteObjectIndex& operator=(const SqliteObjectIndex&) = delete;

  Roe<void> Upsert(const CasObjectMeta& meta) override;
  Roe<std::optional<CasObjectMeta>> Lookup(CasRealm realm, const ByteVector& content_id) const override;
  Roe<void> Remove(CasRealm realm, const ByteVector& content_id) override;
  Roe<void> SetPinned(CasRealm realm, const ByteVector& content_id, bool pinned) override;
  Roe<std::vector<CasObjectMeta>> List(std::optional<CasRealm> realm, std::optional<bool> pinned) const override;

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
