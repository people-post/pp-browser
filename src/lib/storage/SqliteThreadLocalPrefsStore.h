#pragma once

#include "lib/messaging/IThreadLocalPrefsStore.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** IThreadLocalPrefsStore on profile.db, in its own table (additive; `threads` is untouched). */
class SqliteThreadLocalPrefsStore : public IThreadLocalPrefsStore {
public:
  explicit SqliteThreadLocalPrefsStore(std::string profile_db_path);

  /** Its table at the current shape, where missing (a change to it is a ladder step: SqliteChatDbLadders.h). */
  static Roe<void> EnsureSchema(sqlite3* profile_db);

  Roe<std::vector<ThreadLocalPrefs>> List() const override;
  Roe<ThreadLocalPrefs> Get(const std::string& thread_id) const override;
  Roe<void> Set(const ThreadLocalPrefs& prefs) const override;
  Roe<void> Delete(const std::string& thread_id) const override;

private:
  Roe<sqlite3*> OpenDb() const;

  std::string profile_db_path_;
};

} // namespace pbr
