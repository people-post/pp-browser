#include "lib/storage/SqliteChatDbLadders.h"

#include "lib/storage/SqliteCallSessionStore.h"
#include "lib/storage/SqliteGroupRosterStore.h"
#include "lib/storage/SqliteThreadLocalPrefsStore.h"
#include "lib/storage/SqliteThreadSchema.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

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

/** `sql` on `db` with `text` bound to every `?1`, when `table` exists (a store makes its table when it first opens). */
Roe<void> ExecIfTable(sqlite3* db, const char* table, const char* sql, const std::string& text = {}) {
  auto exists = SqliteTableExists(db, table);
  if (!exists) {
    return exists.error();
  }
  if (!*exists) {
    return {};
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    const std::string message = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    return Error(std::string(table) + ": " + message);
  }
  if (sqlite3_bind_parameter_count(stmt) > 0) {
    sqlite3_bind_text(stmt, 1, text.c_str(), -1, SQLITE_TRANSIENT);
  }
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error(std::string(table) + ": " + sqlite3_errmsg(db));
  }
  return {};
}

/** A stored id from before multi-relay MR001: the relay's per-relay `relay:` id (or its `relay_user` kind). */
constexpr const char* kRelayIdMatch = "LIKE 'relay:%'";

/**
 * 5 → 6, Account IDs (multi-relay MR001): the relay's `relay:` ids are gone, and nothing in profile.db can turn one
 * into an Account ID — so what is keyed by one is dropped: a chat's session keys (a new session opens on the next
 * send), group roster rows, invites and owed sends naming one, call participants and invites, applied control-lane
 * ids. A thread whose peer was one keeps its messages and loses its peer (it cannot be sent to). Everything else is
 * keyed by Account ID already and stays.
 */
Roe<void> ProfileDbAccountIds(const SqliteMigrationContext& context) {
  sqlite3* db = context.db;
  const std::string relay = kRelayIdMatch;
  const std::vector<std::pair<const char*, std::string>> statements = {
      {"chat_targets",
       "DELETE FROM chat_targets WHERE peer_identity_kind = 'relay_user' OR peer_identity_value " + relay},
      {"threads",
       "UPDATE threads SET peer_identity_kind = '', peer_identity_value = '' WHERE peer_identity_kind = "
       "'relay_user' OR peer_identity_value " +
           relay},
      {"group_rosters", "DELETE FROM group_rosters WHERE member_identity " + relay},
      {"group_pending_sends", "DELETE FROM group_pending_sends WHERE recipient " + relay},
      {"pending_group_invites",
       "DELETE FROM pending_group_invites WHERE inviter_identity " + relay + " OR invitee_identity " + relay},
      {"call_participants", "DELETE FROM call_participants WHERE identity " + relay},
      {"pending_call_invites",
       "DELETE FROM pending_call_invites WHERE invitee_identity " + relay + " OR inviter_identity " + relay},
      {"control_lane_seen", "DELETE FROM control_lane_seen WHERE sender " + relay},
  };
  for (const auto& [table, sql] : statements) {
    if (auto done = ExecIfTable(db, table, sql.c_str()); !done) {
      return done;
    }
  }
  return {};
}

/**
 * 3 → 4, Account IDs (multi-relay MR001): a message whose sender (or numbering owner) is stored as a `relay:` id is
 * the peer's in a 1:1 chat — it becomes the peer's Account ID (`thread_peer_account`); in a group chat, or a chat
 * whose peer is not known, who sent it cannot be told, and it is dropped. A history watermark keyed by a relay id is
 * dropped (history sync starts that peer over).
 */
Roe<void> ThreadDbAccountIds(const SqliteMigrationContext& context) {
  sqlite3* db = context.db;
  const std::string relay = kRelayIdMatch;
  const std::string& peer = context.thread_peer_account;
  if (!peer.empty()) {
    for (const char* column : {"sender_contact_id", "seq_owner_contact_id"}) {
      const std::string sql = std::string("UPDATE messages SET ") + column + " = ?1 WHERE " + column + " " + relay;
      if (auto done = ExecIfTable(db, "messages", sql.c_str(), peer); !done) {
        return done;
      }
    }
  } else {
    const std::string sql =
        "DELETE FROM messages WHERE sender_contact_id " + relay + " OR seq_owner_contact_id " + relay;
    if (auto done = ExecIfTable(db, "messages", sql.c_str()); !done) {
      return done;
    }
  }
  const std::string sync =
      "DELETE FROM sync_state WHERE peer_identity_kind = 'relay_user' OR peer_identity_value " + relay;
  return ExecIfTable(db, "sync_state", sync.c_str());
}

std::filesystem::path ResetNotePath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "chat_store_reset";
}

} // namespace

const SqliteSchemaLadder& ProfileDbLadder() {
  static const SqliteSchemaLadder ladder{
      "profile.db",
      kSqliteThreadProfileFloorVersion,
      kSqliteThreadProfileUserVersion,
      CreateProfileDb,
      {{4, ProfileDbBaseline}, {5, ProfileDbAccountIds}},
  };
  return ladder;
}

const SqliteSchemaLadder& ThreadDbLadder() {
  static const SqliteSchemaLadder ladder{
      "thread.db",
      kSqliteThreadFloorVersion,
      kSqliteThreadUserVersion,
      [](const SqliteMigrationContext& context) { return SqliteThreadExecSql(context.db, kSqliteThreadSchemaV2); },
      {{2, ThreadDbBaseline}, {3, ThreadDbAccountIds}},
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
