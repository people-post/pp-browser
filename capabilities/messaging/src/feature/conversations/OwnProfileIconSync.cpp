#include "feature/conversations/OwnProfileIconSync.h"

#include "lib/people/ProfileIconCache.h"

#include "common/Logger.h"

#include <utility>

namespace pbr {

OwnProfileIconSync::OwnProfileIconSync(std::string profile_dir, mesh::IDirectoryClient& directory,
                                       std::function<std::string()> own_account_id,
                                       std::function<size_t()> own_device_count, FetchFn fetch,
                                       std::function<int64_t()> now_ms)
    : profile_dir_(std::move(profile_dir)),
      directory_(directory),
      own_account_id_(std::move(own_account_id)),
      own_device_count_(std::move(own_device_count)),
      fetch_(std::move(fetch)),
      now_ms_(std::move(now_ms)) {}

OwnProfileIconSync::Outcome OwnProfileIconSync::Refresh() {
  const std::string account_id = own_account_id_ ? own_account_id_() : std::string();
  if (account_id.empty()) {
    return Outcome::Skipped;
  }
  const int64_t now = now_ms_();
  {
    std::lock_guard lock(mutex_);
    if (running_ || (answered_once_ && now - last_answer_ms_ < kLookupGapMs)) {
      return Outcome::Skipped;
    }
    running_ = true;
  }
  const auto finish = [this](const bool answered, const int64_t at) {
    std::lock_guard lock(mutex_);
    running_ = false;
    if (answered) {
      answered_once_ = true;
      last_answer_ms_ = at;
    }
  };

  auto hit = directory_.LookupByAccount(account_id);
  if (!hit) {
    finish(false, now);
    return Outcome::Failed;
  }
  if (hit->account_id && *hit->account_id != account_id) {
    finish(false, now); // an answer for someone else: take nothing from it
    return Outcome::Failed;
  }
  const std::string self_key = "self";
  if (hit->icon && !hit->icon->url.empty()) {
    {
      std::lock_guard lock(mutex_);
      no_photo_since_ms_ = 0; // the photo is there: a later no-photo answer starts the count over
    }
    if (!ProfileIconNeedsFetch(profile_dir_, self_key, *hit->icon)) {
      finish(true, now);
      return Outcome::Unchanged;
    }
    auto fetched = fetch_(profile_dir_, self_key, *hit->icon);
    if (!fetched) {
      // Reasons only: no ids or urls in the log.
      logging::getLogger("OwnProfileIconSync").warning << "own photo not fetched: " << fetched.error().message;
      finish(false, now);
      return Outcome::Failed;
    }
    finish(true, now);
    return Outcome::Fetched;
  }
  // The directory shows no photo for us: set nowhere, removed on another device, or missing from the backend that
  // answered.
  finish(true, now);
  const auto meta = LoadProfileIconCacheMeta(profile_dir_, self_key);
  if (!meta || ProfileIconLocalPath(profile_dir_, self_key).empty()) {
    return Outcome::Unchanged;
  }
  const size_t devices = own_device_count_ ? own_device_count_() : 0;
  {
    std::lock_guard lock(mutex_);
    if (devices < 2) {
      no_photo_since_ms_ = 0;
      return Outcome::Unchanged; // the only device: nobody else could have removed it
    }
    if (no_photo_since_ms_ == 0) {
      no_photo_since_ms_ = now; // the first such answer in this run: wait for a second one
      return Outcome::Unchanged;
    }
    if (now - no_photo_since_ms_ < kClearGraceMs) {
      return Outcome::Unchanged;
    }
    no_photo_since_ms_ = 0;
  }
  if (auto cleared = ClearProfileIconCache(profile_dir_, self_key); !cleared) {
    return Outcome::Failed;
  }
  return Outcome::Cleared;
}

} // namespace pbr
