#include "lib/storage/SqliteChatDbLadders.h"

#include "lib/storage/SqliteCallSessionStore.h"
#include "lib/storage/SqliteGroupRosterStore.h"
#include "lib/storage/SqliteThreadLocalPrefsStore.h"
#include "lib/storage/SqliteThreadSchema.h"

#include <filesystem>
#include <fstream>
#include <initializer_list>

namespace pbr {

namespace {

struct AddedColumn {
  const char* table;
  const char* column;
  const char* declaration;
};

Roe<void> AddColumnsIfMissing(sqlite3* db, const std::initializer_list<AddedColumn> columns) {
  for (const AddedColumn& added : columns) {
    if (auto done = SqliteAddColumnIfMissing(db, added.table, added.column, added.declaration); !done) {
      return Error(std::string(added.table) + "." + added.column + ": " + done.error().message);
    }
  }
  return {};
}

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
  return SqliteCallSessionStore::EnsureSchema(db);
}

/**
 * 4 → 5, the baseline (D105): a tester's profile.db at schema 4 got its later columns added on each open, so it has
 * some, all or none of them. Each is added where its table exists without it; a table not there yet is made by its
 * store at the current shape. Calls' `session_kind` (the legacy broadcast row kind) is dropped where it was added.
 */
Roe<void> ProfileDbBaseline(const SqliteMigrationContext& context) {
  sqlite3* db = context.db;
  auto added = AddColumnsIfMissing(db, {
                                           {"threads", "listed", "INTEGER NOT NULL DEFAULT 1"},
                                           {"chat_targets", "auto_key_session_id", "TEXT"},
                                           {"chat_targets", "auto_key_init_pending", "TEXT"},
                                           {"chat_targets", "auto_key_peer_heard_ms", "INTEGER"},
                                           {"chat_targets", "restart_pending", "INTEGER NOT NULL DEFAULT 0"},
                                           {"pending_group_invites", "group_title", "TEXT NOT NULL DEFAULT ''"},
                                           {"pending_group_invites", "roster_epoch", "INTEGER NOT NULL DEFAULT 1"},
                                           {"pending_group_invites", "outbound", "INTEGER NOT NULL DEFAULT 0"},
                                           {"pending_group_invites", "owner_identity", "TEXT NOT NULL DEFAULT ''"},
                                           {"group_metadata", "title_version", "INTEGER NOT NULL DEFAULT 0"},
                                           {"group_metadata", "roster_statement_json", "TEXT NOT NULL DEFAULT ''"},
                                           {"group_metadata", "sync_state", "TEXT NOT NULL DEFAULT 'active'"},
                                           {"group_metadata", "sync_request_id", "TEXT NOT NULL DEFAULT ''"},
                                           {"group_metadata", "sync_requested_at", "INTEGER NOT NULL DEFAULT 0"},
                                           {"group_rosters", "added_by", "TEXT NOT NULL DEFAULT ''"},
                                           {"thread_local_prefs", "mute_mentions", "INTEGER NOT NULL DEFAULT 0"},
                                           {"thread_local_prefs", "share_ai_confirmed", "INTEGER NOT NULL DEFAULT 0"},
                                           {"call_sessions", "video_allowed", "INTEGER NOT NULL DEFAULT 0"},
                                           {"call_sessions", "planned_hop", "TEXT"},
                                           {"call_sessions", "planned_hop_ma", "TEXT"},
                                           {"call_sessions", "initiator_identity", "TEXT NOT NULL DEFAULT ''"},
                                           {"pending_call_invites", "video_allowed", "INTEGER NOT NULL DEFAULT 0"},
                                           {"call_participants", "on_hold", "INTEGER NOT NULL DEFAULT 0"},
                                           {"call_participants", "video_paused", "INTEGER NOT NULL DEFAULT 0"},
                                           {"call_participants", "brought_by", "TEXT NOT NULL DEFAULT ''"},
                                       });
  if (!added) {
    return added.error();
  }
  for (const char* table : {"call_sessions", "pending_call_invites"}) {
    if (auto dropped = SqliteDropColumnIfPresent(db, table, "session_kind"); !dropped) {
      return Error(std::string(table) + ".session_kind: " + dropped.error().message);
    }
  }
  return {};
}

/** 2 → 3, the baseline (D105): `messages.sender_device` (M021), added on open before. */
Roe<void> ThreadDbBaseline(const SqliteMigrationContext& context) {
  return AddColumnsIfMissing(context.db, {{"messages", "sender_device", "TEXT"}});
}

std::filesystem::path ResetNotePath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "chat_store_reset";
}

} // namespace

const SqliteSchemaLadder& ProfileDbLadder() {
  static const SqliteSchemaLadder ladder{
      "profile.db",    kSqliteThreadProfileFloorVersion, kSqliteThreadProfileUserVersion,
      CreateProfileDb, {{4, ProfileDbBaseline}},
  };
  return ladder;
}

const SqliteSchemaLadder& ThreadDbLadder() {
  static const SqliteSchemaLadder ladder{
      "thread.db",
      kSqliteThreadFloorVersion,
      kSqliteThreadUserVersion,
      [](const SqliteMigrationContext& context) { return SqliteThreadExecSql(context.db, kSqliteThreadSchemaV2); },
      {{2, ThreadDbBaseline}},
  };
  return ladder;
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
