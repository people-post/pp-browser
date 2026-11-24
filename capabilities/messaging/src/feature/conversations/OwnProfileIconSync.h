#pragma once

#include "common/Error.h"
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"
#include "mesh/types/IDirectoryClient.h"

#include <cstddef>
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
 *
 * Clearing is conservative, because a successful answer without a photo may only mean that the directory backend
 * that answered lacks it: never while the account has one device (nobody else could have removed the photo), and with
 * two or more only after two successful no-photo answers at least kClearGraceMs apart in this run — counted from what
 * was seen, not from the cache's age, so an old photo is not cleared by the first answer after an upgrade. An answer
 * with a photo starts the count over; a failed lookup does not count.
 */
class OwnProfileIconSync {
public:
  /** A lookup that answered is not repeated within this (a burst of locker syncs asks once). */
  static constexpr int64_t kLookupGapMs = 60LL * 1000LL;
  /**
   * Two no-photo answers must be at least this far apart before the cache is cleared (a photo set on this device a
   * moment ago may not be in the directory answer yet; one backend's row may lack it).
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

  /**
   * `own_account_id` returns "" while there is no registered account; `own_device_count` how many devices the
   * account's own card lists (0 when unknown or signed out); `now_ms` is a clock.
   */
  OwnProfileIconSync(std::string profile_dir, mesh::IDirectoryClient& directory,
                     std::function<std::string()> own_account_id, std::function<size_t()> own_device_count,
                     FetchFn fetch, std::function<int64_t()> now_ms);

  Outcome Refresh();

private:
  std::string profile_dir_;
  mesh::IDirectoryClient& directory_;
  std::function<std::string()> own_account_id_;
  std::function<size_t()> own_device_count_;
  FetchFn fetch_;
  std::function<int64_t()> now_ms_;
  std::mutex mutex_;
  int64_t last_answer_ms_ = 0;
  bool answered_once_ = false;
  bool running_ = false;
  /** When this run first saw a no-photo answer since the last answer with one (0: not seen). */
  int64_t no_photo_since_ms_ = 0;
};

} // namespace pbr
