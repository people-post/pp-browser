#include "domain/peers/DirectoryShadowCache.h"

#include "lib/people/ContactIdentity.h"
#include "common/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

std::string AccountKeyFromHit(const mesh::DirectoryHit& hit) {
  if (auto account = PrimaryAccountIdFromHit(hit)) {
    return *account;
  }
  return {};
}

void IndexHit(std::unordered_map<std::string, mesh::DirectoryHit>& by_id, const mesh::DirectoryHit& hit) {
  if (const std::string account = AccountKeyFromHit(hit); !account.empty()) {
    by_id[account] = hit;
  }
}

} // namespace

DirectoryShadowCache::DirectoryShadowCache(mesh::IDirectoryClient& directory) : directory_(directory) {}

void DirectoryShadowCache::SetOnUpdated(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_updated_ = std::move(callback);
}

void DirectoryShadowCache::SetOnHitCached(std::function<void(const mesh::DirectoryHit&)> callback) {
  std::lock_guard lock(mutex_);
  on_hit_cached_ = std::move(callback);
}

std::optional<mesh::DirectoryHit> DirectoryShadowCache::Get(const std::string& identity) const {
  if (identity.empty()) {
    return std::nullopt;
  }
  std::lock_guard lock(mutex_);
  const auto it = by_id_.find(identity);
  if (it == by_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void DirectoryShadowCache::Remove(const std::string& identity) {
  std::lock_guard lock(mutex_);
  const auto it = by_id_.find(identity);
  if (it == by_id_.end()) {
    return;
  }
  const mesh::DirectoryHit hit = it->second; // a copy: erasing the first key invalidates `it`
  for (const std::string& key : {AccountKeyFromHit(hit), identity}) {
    by_id_.erase(key);
  }
}

void DirectoryShadowCache::Clear() {
  std::lock_guard lock(mutex_);
  by_id_.clear();
}

void DirectoryShadowCache::Put(const mesh::DirectoryHit& hit) {
  std::function<void()> notify;
  std::function<void(const mesh::DirectoryHit&)> on_hit;
  mesh::DirectoryHit cached = hit;
  {
    std::lock_guard lock(mutex_);
    IndexHit(by_id_, hit);
    notify = on_updated_;
    on_hit = on_hit_cached_;
  }
  if (on_hit) {
    on_hit(cached);
  }
  if (notify) {
    notify();
  }
}

void DirectoryShadowCache::EnsureLookup(const std::string& identity) {
  if (!IsAccountIdentityValue(identity)) {
    return; // the directory is keyed by Account ID only (multi-relay MR001)
  }
  {
    std::lock_guard lock(mutex_);
    if (by_id_.count(identity) || inflight_.count(identity)) {
      return;
    }
    inflight_.insert(identity);
  }

  AppRuntime::PostWorkerAndReplyOnUI<Roe<mesh::DirectoryHit>>(
      WorkerLane::Normal, [this, identity]() { return directory_.LookupByAccount(identity); },
      [this, identity](Roe<mesh::DirectoryHit> result) {
        std::function<void()> notify;
        std::function<void(const mesh::DirectoryHit&)> on_hit;
        std::optional<mesh::DirectoryHit> cached;
        {
          std::lock_guard lock(mutex_);
          inflight_.erase(identity);
          if (!result) {
            // Until now a failed lookup left no trace: the chat kept showing a bare id and nothing said why.
            logging::getLogger("DirectoryShadow").warning << "directory lookup failed (account " << identity
                                                          << "): " << result.error().message;
          }
          if (result) {
            mesh::DirectoryHit hit = *result;
            bool has_account = false;
            for (const mesh::ContactId& id : hit.ids) {
              if (id.kind == mesh::ContactIdKind::Account && id.value == identity) {
                has_account = true;
                break;
              }
            }
            if (!has_account) {
              hit.ids.push_back({mesh::ContactIdKind::Account, identity, true});
            }
            if (!hit.account_id || hit.account_id->empty()) {
              hit.account_id = identity;
            }
            IndexHit(by_id_, hit);
            cached = hit;
            notify = on_updated_;
            on_hit = on_hit_cached_;
          }
        }
        if (cached && on_hit) {
          on_hit(*cached);
        }
        if (notify) {
          notify();
        }
      });
}

} // namespace pbr
