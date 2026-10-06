#pragma once

#include "common/Error.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** `muted_until` for "always". */
inline constexpr int64_t kThreadMutedForever = std::numeric_limits<int64_t>::max();

/**
 * Per-thread settings that exist only on this device: they are never sent to peers and are not part of
 * the chat protocol. A thread with no row has the defaults.
 */
struct ThreadLocalPrefs {
  std::string thread_id;
  /** Unix ms when the thread was pinned; 0 = not pinned. Later pins sort first. */
  int64_t pinned_at = 0;
  /** Unix ms until which notifications are silenced; 0 = not muted; `kThreadMutedForever` = always. */
  int64_t muted_until = 0;
  bool archived = false;
  /** The user marked the thread unread by hand: the row shows a ring until the thread is opened. */
  bool marked_unread = false;
};

/**
 * Pin / mute / archive / marked-unread per thread, in its own table of profile.db (additive; `threads` is
 * untouched).
 */
class ThreadLocalPrefsStore {
public:
  explicit ThreadLocalPrefsStore(std::string profile_db_path);

  Roe<void> EnsureSchema(sqlite3* profile_db) const;

  /** Rows that differ from the defaults. */
  Roe<std::vector<ThreadLocalPrefs>> List() const;
  /** The thread's settings; defaults when it has no row. */
  Roe<ThreadLocalPrefs> Get(const std::string& thread_id) const;
  /** Stores the settings; a thread back at the defaults keeps no row. */
  Roe<void> Set(const ThreadLocalPrefs& prefs) const;
  Roe<void> Delete(const std::string& thread_id) const;

private:
  Roe<sqlite3*> OpenDb() const;

  std::string profile_db_path_;
};

} // namespace pbr
