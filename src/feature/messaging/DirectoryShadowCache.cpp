#include "feature/messaging/DirectoryShadowCache.h"

#include "base/platform/BrowserThread.h"

namespace pbr {

DirectoryShadowCache::DirectoryShadowCache(IDirectoryClient& directory) : directory_(directory) {}

void DirectoryShadowCache::SetOnUpdated(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_updated_ = std::move(callback);
}

std::optional<DirectoryHit> DirectoryShadowCache::Get(const std::string& relay_user_id) const {
  if (relay_user_id.empty()) {
    return std::nullopt;
  }
  std::lock_guard lock(mutex_);
  const auto it = by_relay_id_.find(relay_user_id);
  if (it == by_relay_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void DirectoryShadowCache::Put(const DirectoryHit& hit) {
  std::string relay_id;
  for (const ContactId& id : hit.ids) {
    if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
      relay_id = id.value;
      break;
    }
  }
  if (relay_id.empty() && !hit.hit_id.empty()) {
    relay_id = hit.hit_id;
  }
  if (relay_id.empty()) {
    return;
  }
  std::function<void()> notify;
  {
    std::lock_guard lock(mutex_);
    by_relay_id_[relay_id] = hit;
    notify = on_updated_;
  }
  if (notify) {
    notify();
  }
}

void DirectoryShadowCache::EnsureLookup(const std::string& relay_user_id) {
  if (relay_user_id.empty()) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (by_relay_id_.count(relay_user_id) || inflight_.count(relay_user_id)) {
      return;
    }
    inflight_.insert(relay_user_id);
  }

  BrowserThread::PostTaskAndReply<Roe<DirectoryHit>>(
      [this, relay_user_id]() { return directory_.LookupRelayUser(relay_user_id); },
      [this, relay_user_id](Roe<DirectoryHit> result) {
        std::function<void()> notify;
        {
          std::lock_guard lock(mutex_);
          inflight_.erase(relay_user_id);
          if (result) {
            DirectoryHit hit = *result;
            bool has_relay = false;
            for (const ContactId& id : hit.ids) {
              if (id.kind == ContactIdKind::RelayUser && id.value == relay_user_id) {
                has_relay = true;
                break;
              }
            }
            if (!has_relay) {
              hit.ids.push_back({ContactIdKind::RelayUser, relay_user_id, true});
            }
            by_relay_id_[relay_user_id] = std::move(hit);
            notify = on_updated_;
          }
        }
        if (notify) {
          notify();
        }
      });
}

} // namespace pbr
