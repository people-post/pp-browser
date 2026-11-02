// CR024: an older profile.db's `chat_targets` becomes `peer_sessions` when the database opens, rows and all.
#include "lib/storage/SqliteThreadStore.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>

namespace pbr {
namespace {

int Count(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* stmt = nullptr;
  int count = -1;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

TEST(PeerSessionsMigrationTest, AnOlderChatTargetsTableIsRenamedWithItsRows) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("pp_peer_sessions_" + util::GenerateUuid());
  std::string db_path;
  {
    SqliteThreadStore store(dir.string());
    ASSERT_TRUE(store.Open());
    db_path = store.ProfileDbPath();
  }
  // Back to the old layout, with a row in it.
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db,
                         "DROP INDEX idx_peer_sessions_local_thread; ALTER TABLE peer_sessions RENAME TO chat_targets; "
                         "CREATE UNIQUE INDEX idx_chat_targets_local_thread ON chat_targets(local_thread_id); "
                         "INSERT INTO chat_targets (peer_identity_kind, peer_identity_value, channel, local_thread_id, "
                         "session_epoch, next_outgoing_seq) VALUES ('account', 'bob', 'e2e', 'thread-1', 3, 7);",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  sqlite3_close(db);

  {
    SqliteThreadStore store(dir.string());
    ASSERT_TRUE(store.Open());
  }
  ASSERT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
  EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM peer_sessions WHERE peer_identity_value = 'bob' AND session_epoch = 3;"),
            1);
  EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'chat_targets';"), 0);
  EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'idx_chat_targets_local_thread';"), 0);
  EXPECT_EQ(Count(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'idx_peer_sessions_local_thread';"), 1);
  sqlite3_close(db);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

} // namespace
} // namespace pbr
