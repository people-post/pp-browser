#include "feature/conversations/DirectoryShadowCache.h"

#include "domain/people/ContactIdentity.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string AccountKeyFromHit(const DirectoryHit& hit) {
  if (auto account = PrimaryAccountIdFromHit(hit)) {
    return *account;
  }
  return {};
}

std::string RelayKeyFromHit(const DirectoryHit& hit) {
  if (auto relay = PrimaryRelayIdFromHit(hit)) {
    return *relay;
  }
  if (!hit.hit_id.empty() && hit.hit_id.rfind("relay:", 0) == 0) {
    return hit.hit_id;
  }
  return {};
}

void IndexHit(std::unordered_map<std::string, DirectoryHit>& by_id, const DirectoryHit& hit) {
  const std::string account = AccountKeyFromHit(hit);
  const std::string relay = RelayKeyFromHit(hit);
  if (!account.empty()) {
    by_id[account] = hit;
  }
  if (!relay.empty()) {
    by_id[relay] = hit;
  }
}

} // namespace

DirectoryShadowCache::DirectoryShadowCache(IDirectoryClient& directory) : directory_(directory) {}

void DirectoryShadowCache::SetOnUpdated(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_updated_ = std::move(callback);
}

void DirectoryShadowCache::SetOnHitCached(std::function<void(const DirectoryHit&)> callback) {
  std::lock_guard lock(mutex_);
  on_hit_cached_ = std::move(callback);
}

std::optional<DirectoryHit> DirectoryShadowCache::Get(const std::string& identity) const {
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

void DirectoryShadowCache::Put(const DirectoryHit& hit) {
  std::function<void()> notify;
  std::function<void(const DirectoryHit&)> on_hit;
  DirectoryHit cached = hit;
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
  if (identity.empty()) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (by_id_.count(identity) || inflight_.count(identity)) {
      return;
    }
    inflight_.insert(identity);
  }

  const bool by_account = IsAccountIdentityValue(identity);
  AppRuntime::PostWorkerAndReplyOnUI<Roe<DirectoryHit>>(
      WorkerLane::Normal,
      [this, identity, by_account]() {
        return by_account ? directory_.LookupByAccount(identity) : directory_.LookupRelayUser(identity);
      },
      [this, identity, by_account](Roe<DirectoryHit> result) {
        std::function<void()> notify;
        std::function<void(const DirectoryHit&)> on_hit;
        std::optional<DirectoryHit> cached;
        {
          std::lock_guard lock(mutex_);
          inflight_.erase(identity);
          if (result) {
            DirectoryHit hit = *result;
            if (by_account) {
              bool has_account = false;
              for (const ContactId& id : hit.ids) {
                if (id.kind == ContactIdKind::Account && id.value == identity) {
                  has_account = true;
                  break;
                }
              }
              if (!has_account) {
                hit.ids.push_back({ContactIdKind::Account, identity, true});
              }
              if (!hit.account_id || hit.account_id->empty()) {
                hit.account_id = identity;
              }
            } else {
              bool has_relay = false;
              for (const ContactId& id : hit.ids) {
                if (id.kind == ContactIdKind::RelayUser && id.value == identity) {
                  has_relay = true;
                  break;
                }
              }
              if (!has_relay) {
                hit.ids.push_back({ContactIdKind::RelayUser, identity, false});
              }
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
