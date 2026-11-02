#include "lib/content/testing/InMemoryObjectIndex.h"

#include "foundation/crypto/CryptoUtil.h"

#include <algorithm>
#include <chrono>

namespace pbr {
namespace {

Roe<std::string> IdHex(const ByteVector& content_id) {
  if (content_id.size() != kCasContentIdSize) {
    return Error("Invalid CAS content id");
  }
  return BytesToHex(content_id);
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

Roe<void> InMemoryObjectIndex::Upsert(const CasObjectMeta& meta) {
  auto hex = IdHex(meta.content_id);
  if (!hex) {
    return hex.error();
  }
  std::lock_guard lock(mutex_);
  CasObjectMeta row = meta;
  if (const auto it = rows_.find({meta.realm, *hex}); it != rows_.end()) {
    row.created_at_ms = it->second.created_at_ms;
  } else if (row.created_at_ms <= 0) {
    row.created_at_ms = NowMs();
  }
  rows_[{meta.realm, *hex}] = std::move(row);
  return {};
}

Roe<std::optional<CasObjectMeta>> InMemoryObjectIndex::Lookup(const CasRealm realm,
                                                              const ByteVector& content_id) const {
  auto hex = IdHex(content_id);
  if (!hex) {
    return hex.error();
  }
  std::lock_guard lock(mutex_);
  const auto it = rows_.find({realm, *hex});
  if (it == rows_.end()) {
    return std::optional<CasObjectMeta>{};
  }
  return std::optional<CasObjectMeta>{it->second};
}

Roe<void> InMemoryObjectIndex::Remove(const CasRealm realm, const ByteVector& content_id) {
  auto hex = IdHex(content_id);
  if (!hex) {
    return hex.error();
  }
  std::lock_guard lock(mutex_);
  rows_.erase({realm, *hex});
  return {};
}

Roe<void> InMemoryObjectIndex::SetPinned(const CasRealm realm, const ByteVector& content_id, const bool pinned) {
  auto hex = IdHex(content_id);
  if (!hex) {
    return hex.error();
  }
  std::lock_guard lock(mutex_);
  if (const auto it = rows_.find({realm, *hex}); it != rows_.end()) {
    it->second.pinned = pinned;
  }
  return {};
}

Roe<std::vector<CasObjectMeta>> InMemoryObjectIndex::List(const std::optional<CasRealm> realm,
                                                          const std::optional<bool> pinned) const {
  std::vector<std::pair<std::string, CasObjectMeta>> matched;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [key, row] : rows_) {
      if ((!realm || row.realm == *realm) && (!pinned || row.pinned == *pinned)) {
        matched.emplace_back(key.second, row);
      }
    }
  }
  std::stable_sort(matched.begin(), matched.end(), [](const auto& a, const auto& b) {
    if (a.second.created_at_ms != b.second.created_at_ms) {
      return a.second.created_at_ms > b.second.created_at_ms;
    }
    return a.first < b.first;
  });
  std::vector<CasObjectMeta> rows;
  rows.reserve(matched.size());
  for (auto& [hex, row] : matched) {
    rows.push_back(std::move(row));
  }
  return rows;
}

std::shared_ptr<CasStore> MakeInMemoryIndexedCasStore(std::string profile_dir, std::string profile_id) {
  return std::make_shared<CasStore>(std::move(profile_dir), std::move(profile_id),
                                    std::make_unique<InMemoryObjectIndex>());
}

} // namespace pbr
