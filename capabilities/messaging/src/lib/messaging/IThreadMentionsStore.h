#pragma once

#include "common/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The unread live group messages that mention this account (group-chat G015 marker): one per message. Written on
 * arrival (or when a held mention is decided), never by history sync; a thread's are read when it is opened (only its
 * newest is kept, read, for Home's widget) and go when it is deleted or cleared, a message's when it is deleted. Per
 * device. The app's storage keeps them (CR024); tests use an in-memory one.
 */
class IThreadMentionsStore {
public:
  /** One mention: its chat, its message and when it was sent. */
  struct Mention {
    std::string thread_id;
    std::string message_id;
    int64_t created_at = 0;

    bool operator==(const Mention&) const = default;
  };

  virtual ~IThreadMentionsStore() = default;

  virtual Roe<void> Add(const std::string& thread_id, const std::string& message_id, int64_t created_at) const = 0;
  /** The thread was opened: its mentions stop being unread; the newest stays known to Latest. */
  virtual Roe<void> MarkThreadRead(const std::string& thread_id) const = 0;
  /** The thread or its history is gone: all of its mentions go, read or not. */
  virtual Roe<void> DropThread(const std::string& thread_id) const = 0;
  virtual Roe<void> DropMessage(const std::string& thread_id, const std::string& message_id) const = 0;
  /** The threads with a mention unread ("You were mentioned"). */
  virtual Roe<std::set<std::string>> Threads() const = 0;
  /** How many unread mentions each thread with one has (Home's "Mentions" widget counts them). */
  virtual Roe<std::map<std::string, int>> Counts() const = 0;
  /** The oldest unread mention of a thread (where opening it can jump to); none when it has none. */
  virtual Roe<std::optional<std::string>> Oldest(const std::string& thread_id) const = 0;
  /** The newest mention over every chat, read or not (Home's "Mentions" widget shows a recent one); none: none. */
  virtual Roe<std::optional<Mention>> Latest() const = 0;
};

} // namespace pbr
