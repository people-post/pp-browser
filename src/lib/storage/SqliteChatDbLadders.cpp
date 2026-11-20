#include "lib/storage/SqliteChatDbLadders.h"

#include "lib/storage/SqliteCallSessionStore.h"
#include "lib/storage/SqliteGroupRosterStore.h"
#include "lib/storage/SqlitePeerSessionStore.h"
#include "lib/storage/SqliteThreadLocalPrefsStore.h"
#include "lib/storage/SqliteThreadSchema.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

namespace {

Roe<void> CreateProfileDb(const SqliteMigrationContext& context) {
  sqlite3* db = context.db;
  if (auto schema = SqliteThreadExecSql(db, kSqliteThreadProfileSchemaV1); !schema) {
    return schema.error();
  }
  if (auto groups = SqliteGroupRosterStore::EnsureSchema(db); !groups) {
    return groups.error();
  }
  if (auto prefs = SqliteThreadLocalPrefsStore::EnsureSchema(db); !prefs) {
    return prefs.error();
  }
  if (auto sessions = SqlitePeerSessionStore::EnsureSchema(db); !sessions) {
    return sessions.error();
  }
  return SqliteCallSessionStore::EnsureSchema(db);
}

std::filesystem::path ResetNotePath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "chat_store_reset";
}

} // namespace

const SqliteSchemaLadder& ProfileDbLadder() {
  static const SqliteSchemaLadder ladder{
      "profile.db", kSqliteThreadProfileFloorVersion, kSqliteThreadProfileUserVersion, CreateProfileDb, {},
  };
  return ladder;
}

const SqliteSchemaLadder& ThreadDbLadder() {
  static const SqliteSchemaLadder ladder{
      "thread.db",
      kSqliteThreadFloorVersion,
      kSqliteThreadUserVersion,
      [](const SqliteMigrationContext& context) { return SqliteThreadExecSql(context.db, kSqliteThreadSchemaV2); },
      {},
  };
  return ladder;
}

Roe<std::vector<PeerSessionRecord>> ReadSessionsKeptBelowFloor(sqlite3* old_db) {
  auto stored = SqlitePeerSessionStore::ReadStored(old_db);
  if (!stored) {
    return stored.error();
  }
  return SessionsAChatResetKeeps(std::move(*stored));
}

Roe<void> WriteSessionsKeptBelowFloor(sqlite3* new_db, const std::vector<PeerSessionRecord>& kept) {
  if (auto begun = SqliteThreadExecSql(new_db, "BEGIN IMMEDIATE;"); !begun) {
    return begun.error();
  }
  for (const PeerSessionRecord& sealed : kept) {
    if (auto written = SqlitePeerSessionStore::WriteStored(new_db, sealed); !written) {
      (void)SqliteThreadExecSql(new_db, "ROLLBACK;");
      return written.error();
    }
  }
  if (auto committed = SqliteThreadExecSql(new_db, "COMMIT;"); !committed) {
    (void)SqliteThreadExecSql(new_db, "ROLLBACK;");
    return committed.error();
  }
  return {};
}

std::optional<ChatStoreProblem> ChatStoreProblemFor(const Roe<SqliteLadderOutcome>& outcome) {
  if (!outcome) {
    return ChatStoreProblem::NotUpgraded;
  }
  if (*outcome == SqliteLadderOutcome::Newer) {
    return ChatStoreProblem::Newer;
  }
  return std::nullopt;
}

void NoteChatStoreReset(const std::string& profile_data_dir, const std::string& what) {
  std::ofstream out(ResetNotePath(profile_data_dir), std::ios::app);
  out << what << "\n";
}

bool TakeChatStoreResetNotice(const std::string& profile_data_dir) {
  std::error_code ec;
  return std::filesystem::remove(ResetNotePath(profile_data_dir), ec) && !ec;
}

} // namespace pbr
