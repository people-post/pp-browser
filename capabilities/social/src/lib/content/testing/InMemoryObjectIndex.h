#pragma once

// The content index in memory, for social's tests and the tests above it (CR024). Never linked into the app or
// pp-node.

#include "lib/content/CasStore.h"
#include "lib/content/IObjectIndex.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/** IObjectIndex in memory, as the SQLite one: a row's creation time survives an update; newest first. */
class InMemoryObjectIndex : public IObjectIndex {
public:
  Roe<void> Upsert(const CasObjectMeta& meta) override;
  Roe<std::optional<CasObjectMeta>> Lookup(CasRealm realm, const ByteVector& content_id) const override;
  Roe<void> Remove(CasRealm realm, const ByteVector& content_id) override;
  Roe<void> SetPinned(CasRealm realm, const ByteVector& content_id, bool pinned) override;
  Roe<std::vector<CasObjectMeta>> List(std::optional<CasRealm> realm, std::optional<bool> pinned) const override;

private:
  mutable std::mutex mutex_;
  /** By (realm, content id hex). */
  std::map<std::pair<CasRealm, std::string>, CasObjectMeta> rows_;
};

/** A content store on `profile_dir` (its blocks on disk) with its index in memory. */
std::shared_ptr<CasStore> MakeInMemoryIndexedCasStore(std::string profile_dir, std::string profile_id);

} // namespace pbr
