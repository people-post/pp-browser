#pragma once

#include "common/Error.h"
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"
#include "mesh/types/IDirectoryClient.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace pbr {

/**
 * Keeps this device's own profile photo (the cache entry `self`) current when the photo is set on ANOTHER device of
 * the account. The photo lives in the relay's directory entry of the account; the picking device writes the `self`
 * cache itself when it uploads, but a linked or restored device (and every sibling after a change) has no such cache
 * and, unlike a contact's photo, nothing ever looked its own entry up. One refresh: look the own Account ID up in the
 * directory, fetch the photo when its url / blob id is not what the cache holds, clear the cache when the entry has
 * no photo any more. Blocking: call it off the UI thread.
 */
class OwnProfileIconSync {
public:
  /** A lookup that answered is not repeated within this (a burst of locker syncs asks once). */
  static constexpr int64_t kLookupGapMs = 60LL * 1000LL;
  /**
   * A cache younger than this is never cleared because the directory shows no photo: a photo set on this device a
   * moment ago may not be in the directory answer yet.
   */
  static constexpr int64_t kClearGraceMs = 10LL * 60LL * 1000LL;

  enum class Outcome {
    Skipped,   // nothing to do: no account yet, or asked lately
    Failed,    // the lookup or the download failed; the next refresh asks again
    Unchanged, // the cache already is what the directory shows
    Fetched,   // a new photo is cached
    Cleared    // the photo was removed elsewhere: the cache is gone
  };

  using FetchFn = std::function<Roe<void>(const std::string& profile_dir, const std::string& cache_key,
                                          const mesh::ProfileIconRef& icon)>;

  /** `own_account_id` returns "" while there is no registered account; `now_ms` is a clock. */
  OwnProfileIconSync(std::string profile_dir, mesh::IDirectoryClient& directory,
                     std::function<std::string()> own_account_id, FetchFn fetch, std::function<int64_t()> now_ms);

  Outcome Refresh();

private:
  std::string profile_dir_;
  mesh::IDirectoryClient& directory_;
  std::function<std::string()> own_account_id_;
  FetchFn fetch_;
  std::function<int64_t()> now_ms_;
  std::mutex mutex_;
  int64_t last_answer_ms_ = 0;
  bool answered_once_ = false;
  bool running_ = false;
};

} // namespace pbr
