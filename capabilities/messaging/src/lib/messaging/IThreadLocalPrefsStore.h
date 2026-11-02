#pragma once

#include "common/Error.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

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
  /** While muted, mentions are silent too (G015: by default a mention alerts in a muted group). */
  bool mute_mentions = false;
  /** The user ticked "don't ask again" when sharing the chat with the AI ("@ai+" / "@ai++"): no confirm for this chat. */
  bool share_ai_confirmed = false;
};

/** Where a device keeps its per-thread settings (CR024); the app's storage keeps them, tests an in-memory one. */
class IThreadLocalPrefsStore {
public:
  virtual ~IThreadLocalPrefsStore() = default;

  /** Rows that differ from the defaults. */
  virtual Roe<std::vector<ThreadLocalPrefs>> List() const = 0;
  /** The thread's settings; defaults when it has no row. */
  virtual Roe<ThreadLocalPrefs> Get(const std::string& thread_id) const = 0;
  /** Stores the settings; a thread back at the defaults keeps no row. */
  virtual Roe<void> Set(const ThreadLocalPrefs& prefs) const = 0;
  virtual Roe<void> Delete(const std::string& thread_id) const = 0;
};

} // namespace pbr
