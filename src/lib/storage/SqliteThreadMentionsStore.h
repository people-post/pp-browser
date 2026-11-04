#pragma once

#include "lib/messaging/IThreadMentionsStore.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** IThreadMentionsStore on profile.db (`thread_mentions`, additive); each call opens its own handle. */
class SqliteThreadMentionsStore : public IThreadMentionsStore {
public:
  explicit SqliteThreadMentionsStore(std::string profile_db_path);

  Roe<void> EnsureSchema(sqlite3* profile_db) const;

  Roe<void> Add(const std::string& thread_id, const std::string& message_id, int64_t created_at) const override;
  Roe<void> DropThread(const std::string& thread_id) const override;
  Roe<void> DropMessage(const std::string& thread_id, const std::string& message_id) const override;
  Roe<std::set<std::string>> Threads() const override;
  Roe<std::map<std::string, int>> Counts() const override;
  Roe<std::optional<std::string>> Oldest(const std::string& thread_id) const override;

private:
  Roe<sqlite3*> OpenDb() const;
  Roe<void> Exec(const char* sql, const std::string& a, const std::string& b = {}, int64_t n = 0) const;

  std::string profile_db_path_;
};

} // namespace pbr
