#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The unread live group messages that mention this account (group-chat G015 marker): one per message. Written on
 * arrival (or when a held mention is decided), never by history sync; a thread's go when it is opened or deleted, a
 * message's when it is deleted. Per device. The app's storage keeps them (CR024); tests use an in-memory one.
 */
class IThreadMentionsStore {
public:
  virtual ~IThreadMentionsStore() = default;

  virtual Roe<void> Add(const std::string& thread_id, const std::string& message_id, int64_t created_at) const = 0;
  virtual Roe<void> DropThread(const std::string& thread_id) const = 0;
  virtual Roe<void> DropMessage(const std::string& thread_id, const std::string& message_id) const = 0;
  /** The threads with a mention unread ("You were mentioned"). */
  virtual Roe<std::set<std::string>> Threads() const = 0;
  /** The oldest unread mention of a thread (where opening it can jump to); none when it has none. */
  virtual Roe<std::optional<std::string>> Oldest(const std::string& thread_id) const = 0;
};

} // namespace pbr
