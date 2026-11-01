#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * The unread live group messages that mention this account (group-chat G015 marker): one row per message, in its
 * own table of profile.db (`thread_mentions`, additive). Written on arrival (or when a held mention is decided),
 * never by history sync; a thread's rows go when it is opened or deleted, a message's when it is deleted. Per
 * device. Any thread (each call opens its own handle, as ThreadLocalPrefsStore does).
 */
class ThreadMentionsStore {
public:
  explicit ThreadMentionsStore(std::string profile_db_path);

  Roe<void> EnsureSchema(sqlite3* profile_db) const;

  Roe<void> Add(const std::string& thread_id, const std::string& message_id, int64_t created_at) const;
  Roe<void> DropThread(const std::string& thread_id) const;
  Roe<void> DropMessage(const std::string& thread_id, const std::string& message_id) const;
  /** The threads with a mention unread ("You were mentioned"). */
  Roe<std::set<std::string>> Threads() const;
  /** The oldest unread mention of a thread (where opening it can jump to); none when it has none. */
  Roe<std::optional<std::string>> Oldest(const std::string& thread_id) const;

private:
  Roe<sqlite3*> OpenDb() const;
  Roe<void> Exec(const char* sql, const std::string& a, const std::string& b = {}, int64_t n = 0) const;

  std::string profile_db_path_;
};

} // namespace pbr
