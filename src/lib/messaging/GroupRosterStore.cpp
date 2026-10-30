#include "lib/messaging/GroupRosterStore.h"

#include "lib/messaging/GroupMembershipCodec.h"

#include "common/Utilities.h"

#include <sqlite3.h>
#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr const char* kGroupSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS group_targets (
  group_id TEXT PRIMARY KEY,
  local_thread_id TEXT NOT NULL,
  session_epoch INTEGER NOT NULL DEFAULT 1,
  next_outgoing_seq INTEGER NOT NULL DEFAULT 1
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_group_targets_thread ON group_targets(local_thread_id);

-- G010/G011: our seq for each member's copy, numbered in the pair (its session epoch). Kept when we leave:
-- a re-join on this device continues the count; a lost count is the pair's restart (D014).
CREATE TABLE IF NOT EXISTS group_member_seq (
  group_id TEXT NOT NULL,
  member_identity TEXT NOT NULL,
  session_epoch INTEGER NOT NULL,
  next_seq INTEGER NOT NULL,
  PRIMARY KEY (group_id, member_identity)
);

CREATE TABLE IF NOT EXISTS group_metadata (
  group_id TEXT PRIMARY KEY,
  owner_identity TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  roster_epoch INTEGER NOT NULL DEFAULT 1,
  policy_json TEXT NOT NULL DEFAULT '{}',
  title_version INTEGER NOT NULL DEFAULT 0,
  roster_statement_json TEXT NOT NULL DEFAULT '',
  sync_state TEXT NOT NULL DEFAULT 'active',
  sync_request_id TEXT NOT NULL DEFAULT '',
  sync_requested_at INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS group_rosters (
  group_id TEXT NOT NULL,
  member_identity TEXT NOT NULL,
  contact_id TEXT NOT NULL DEFAULT '',
  role TEXT NOT NULL DEFAULT 'member',
  joined_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (group_id, member_identity)
);

CREATE TABLE IF NOT EXISTS pending_group_invites (
  invite_nonce TEXT PRIMARY KEY,
  group_id TEXT NOT NULL,
  group_title TEXT NOT NULL DEFAULT '',
  inviter_identity TEXT NOT NULL,
  invitee_identity TEXT NOT NULL,
  roster_epoch INTEGER NOT NULL DEFAULT 1,
  status TEXT NOT NULL DEFAULT 'pending',
  expires_at INTEGER,
  created_at INTEGER NOT NULL,
  outbound INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_pending_invites_created ON pending_group_invites(created_at);
)sql";

std::string InviteStatusToString(const InviteStatus status) {
  switch (status) {
  case InviteStatus::Pending:
    return "pending";
  case InviteStatus::Accepted:
    return "accepted";
  case InviteStatus::Declined:
    return "declined";
  case InviteStatus::Expired:
    return "expired";
  case InviteStatus::Blocked:
    return "blocked";
  }
  return "pending";
}

InviteStatus InviteStatusFromString(const std::string& value) {
  if (value == "accepted") {
    return InviteStatus::Accepted;
  }
  if (value == "declined") {
    return InviteStatus::Declined;
  }
  if (value == "expired") {
    return InviteStatus::Expired;
  }
  if (value == "blocked") {
    return InviteStatus::Blocked;
  }
  return InviteStatus::Pending;
}

} // namespace

GroupRosterStore::GroupRosterStore(std::string profile_db_path) : profile_db_path_(std::move(profile_db_path)) {}

Roe<void> GroupRosterStore::EnsureSchema(sqlite3* profile_db) const {
  char* err = nullptr;
  if (sqlite3_exec(profile_db, kGroupSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "group schema failed";
    sqlite3_free(err);
    return Error(message);
  }
  // Legacy profile.db may lack snapshot columns on pending_group_invites.
  (void)sqlite3_exec(profile_db, "ALTER TABLE pending_group_invites ADD COLUMN group_title TEXT NOT NULL DEFAULT '';",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE pending_group_invites ADD COLUMN roster_epoch INTEGER NOT NULL DEFAULT 1;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE pending_group_invites ADD COLUMN outbound INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE group_metadata ADD COLUMN title_version INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  // G012 / G013: the owner-signed roster we hold, and a restored group's member check.
  for (const char* column : {"roster_statement_json TEXT NOT NULL DEFAULT ''", "sync_state TEXT NOT NULL DEFAULT 'active'",
                             "sync_request_id TEXT NOT NULL DEFAULT ''",
                             "sync_requested_at INTEGER NOT NULL DEFAULT 0"}) {
    const std::string sql = std::string("ALTER TABLE group_metadata ADD COLUMN ") + column + ";";
    (void)sqlite3_exec(profile_db, sql.c_str(), nullptr, nullptr, nullptr);
  }
  return {};
}

void GroupRosterStore::SetOnChanged(std::function<void(const std::string& group_id)> on_changed) {
  on_changed_ = std::move(on_changed);
}

void GroupRosterStore::NotifyChanged(const std::string& group_id) const {
  if (on_changed_) {
    on_changed_(group_id);
  }
}

std::unique_lock<std::recursive_mutex> GroupRosterStore::LockForUpdate() {
  static std::recursive_mutex mutex;
  return std::unique_lock<std::recursive_mutex>(mutex);
}

bool GroupRosterStore::OpenDb(sqlite3** db) const {
  if (sqlite3_open(profile_db_path_.c_str(), db) != SQLITE_OK) {
    return false;
  }
  sqlite3_busy_timeout(*db, 5000);
  return true;
}

Roe<void> GroupRosterStore::UpsertMetadata(const GroupMetadata& metadata) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db for group metadata");
  }
  if (auto schema = EnsureSchema(db); !schema) {
    sqlite3_close(db);
    return schema.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO group_metadata (group_id, owner_identity, title, roster_epoch, policy_json, title_version, "
      "roster_statement_json, sync_state, sync_request_id, sync_requested_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(group_id) DO UPDATE SET owner_identity=excluded.owner_identity, title=excluded.title, "
      "roster_epoch=excluded.roster_epoch, policy_json=excluded.policy_json, title_version=excluded.title_version, "
      "roster_statement_json=excluded.roster_statement_json, sync_state=excluded.sync_state, "
      "sync_request_id=excluded.sync_request_id, sync_requested_at=excluded.sync_requested_at;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group metadata upsert");
  }
  const std::string policy_json = GroupMembershipCodec::EncodeGroupPolicy(metadata.policy);
  sqlite3_bind_text(stmt, 1, metadata.group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, metadata.owner_identity.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, metadata.title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(metadata.roster_epoch));
  sqlite3_bind_text(stmt, 5, policy_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(metadata.title_version));
  sqlite3_bind_text(stmt, 7, metadata.roster_statement_json.c_str(), -1, SQLITE_TRANSIENT);
  const std::string sync_state = GroupSyncStateToString(metadata.sync_state);
  sqlite3_bind_text(stmt, 8, sync_state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, metadata.sync_request_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(metadata.sync_requested_at));
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Error("Failed to upsert group metadata");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  NotifyChanged(metadata.group_id);
  return {};
}

Roe<std::optional<GroupMetadata>> GroupRosterStore::LoadMetadata(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT owner_identity, title, roster_epoch, policy_json, title_version, roster_statement_json, "
                             "sync_state, sync_request_id, sync_requested_at FROM group_metadata WHERE "
                             "group_id = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group metadata load");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<GroupMetadata> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    GroupMetadata metadata;
    metadata.group_id = group_id;
    metadata.owner_identity = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    metadata.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    metadata.roster_epoch = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
    if (sqlite3_column_text(stmt, 3)) {
      if (auto policy = GroupMembershipCodec::DecodeGroupPolicy(
              reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)))) {
        metadata.policy = *policy;
      }
    }
    metadata.title_version = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
    const auto text = [stmt](int column) {
      const unsigned char* value = sqlite3_column_text(stmt, column);
      return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
    };
    metadata.roster_statement_json = text(5);
    metadata.sync_state = GroupSyncStateFromString(text(6));
    metadata.sync_request_id = text(7);
    metadata.sync_requested_at = static_cast<int64_t>(sqlite3_column_int64(stmt, 8));
    if (auto epoch = GetGroupSessionEpoch(group_id)) {
      metadata.session_epoch = *epoch;
    }
    result = std::move(metadata);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return result;
}

Roe<std::vector<std::string>> GroupRosterStore::ListGroupIds() const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT group_id FROM group_metadata ORDER BY group_id;", -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group list");
  }
  std::vector<std::string> ids;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (const unsigned char* id = sqlite3_column_text(stmt, 0)) {
      ids.emplace_back(reinterpret_cast<const char*>(id));
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return ids;
}

Roe<void> GroupRosterStore::UpsertMember(const std::string& group_id, const GroupRosterMember& member) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO group_rosters (group_id, member_identity, contact_id, role, joined_at) VALUES (?, ?, ?, ?, ?) "
      "ON CONFLICT(group_id, member_identity) DO UPDATE SET contact_id=excluded.contact_id, role=excluded.role, "
      "joined_at=excluded.joined_at;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare roster upsert");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, member.member_identity.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, member.contact_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, MemberRoleToString(member.role).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, member.joined_at);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Error("Failed to upsert roster member");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  NotifyChanged(group_id);
  return {};
}

Roe<void> GroupRosterStore::RemoveMember(const std::string& group_id, const std::string& member_identity) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM group_rosters WHERE group_id = ? AND member_identity = ?;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare roster delete");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, member_identity.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  NotifyChanged(group_id);
  return {};
}

Roe<std::vector<GroupRosterMember>> GroupRosterStore::ListMembers(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT member_identity, contact_id, role, joined_at FROM group_rosters WHERE group_id = ?;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare roster list");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<GroupRosterMember> members;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    GroupRosterMember member;
    member.member_identity = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (sqlite3_column_text(stmt, 1)) {
      member.contact_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    }
    member.role = MemberRoleFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
    member.joined_at = sqlite3_column_int64(stmt, 3);
    members.push_back(std::move(member));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return members;
}

Roe<std::optional<GroupRosterMember>> GroupRosterStore::FindMember(const std::string& group_id,
                                                                 const std::string& member_identity) const {
  auto members = ListMembers(group_id);
  if (!members) {
    return members.error();
  }
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity == member_identity) {
      return Roe<std::optional<GroupRosterMember>>(member);
    }
  }
  return Roe<std::optional<GroupRosterMember>>(std::optional<GroupRosterMember>{});
}

Roe<bool> GroupRosterStore::IsMember(const std::string& group_id, const std::string& member_identity) const {
  auto member = FindMember(group_id, member_identity);
  if (!member) {
    return member.error();
  }
  return member->has_value();
}

Roe<void> GroupRosterStore::UpsertPendingInvite(const PendingGroupInvite& invite) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      // A stored invite is never rewritten by another one with the same nonce (status changes go
      // through UpdateInviteStatus).
      "INSERT INTO pending_group_invites (invite_nonce, group_id, group_title, inviter_identity, invitee_identity, "
      "roster_epoch, status, expires_at, created_at, outbound) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(invite_nonce) DO NOTHING;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare invite upsert");
  }
  sqlite3_bind_text(stmt, 1, invite.invite_nonce.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, invite.group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, invite.group_title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, invite.inviter_identity.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, invite.invitee_identity.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(invite.roster_epoch));
  sqlite3_bind_text(stmt, 7, InviteStatusToString(invite.status).c_str(), -1, SQLITE_TRANSIENT);
  if (invite.expires_at) {
    sqlite3_bind_int64(stmt, 8, *invite.expires_at);
  } else {
    sqlite3_bind_null(stmt, 8);
  }
  sqlite3_bind_int64(stmt, 9, invite.created_at);
  sqlite3_bind_int(stmt, 10, invite.outbound ? 1 : 0);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Error("Failed to upsert invite");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return {};
}

Roe<std::optional<PendingGroupInvite>> GroupRosterStore::LoadPendingInvite(const std::string& invite_nonce) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT group_id, group_title, inviter_identity, invitee_identity, roster_epoch, status, "
                         "expires_at, created_at, outbound FROM pending_group_invites WHERE invite_nonce = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare invite load");
  }
  sqlite3_bind_text(stmt, 1, invite_nonce.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<PendingGroupInvite> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    PendingGroupInvite invite;
    invite.invite_nonce = invite_nonce;
    invite.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (sqlite3_column_text(stmt, 1)) {
      invite.group_title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    }
    invite.inviter_identity = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    invite.invitee_identity = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    invite.roster_epoch = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
    invite.status = InviteStatusFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
      invite.expires_at = sqlite3_column_int64(stmt, 6);
    }
    invite.created_at = sqlite3_column_int64(stmt, 7);
    invite.outbound = sqlite3_column_int(stmt, 8) != 0;
    result = std::move(invite);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return result;
}

Roe<void> GroupRosterStore::UpdateInviteStatus(const std::string& invite_nonce, const InviteStatus status) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "UPDATE pending_group_invites SET status = ? WHERE invite_nonce = ?;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare invite status update");
  }
  sqlite3_bind_text(stmt, 1, InviteStatusToString(status).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, invite_nonce.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return {};
}

Roe<std::vector<PendingGroupInvite>> GroupRosterStore::ListOutboundPendingInvites(const std::string& group_id,
                                                                                  const int64_t now_ms) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT invite_nonce, group_title, inviter_identity, invitee_identity, roster_epoch, "
                         "expires_at, created_at FROM pending_group_invites WHERE group_id = ? AND outbound = 1 "
                         "AND status = 'pending' AND (expires_at IS NULL OR expires_at > ?) "
                         "ORDER BY created_at DESC;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare outbound invite list");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, now_ms);
  std::vector<PendingGroupInvite> invites;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PendingGroupInvite invite;
    invite.group_id = group_id;
    invite.invite_nonce = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (sqlite3_column_text(stmt, 1)) {
      invite.group_title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    }
    invite.inviter_identity = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    invite.invitee_identity = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    invite.roster_epoch = static_cast<uint64_t>(sqlite3_column_int64(stmt, 4));
    invite.status = InviteStatus::Pending;
    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
      invite.expires_at = sqlite3_column_int64(stmt, 5);
    }
    invite.created_at = sqlite3_column_int64(stmt, 6);
    invite.outbound = true;
    const bool seen = std::any_of(invites.begin(), invites.end(), [&](const PendingGroupInvite& other) {
      return other.invitee_identity == invite.invitee_identity;
    });
    if (!seen) {
      invites.push_back(std::move(invite));
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return invites;
}

Roe<size_t> GroupRosterStore::CountPendingInvitesSince(const int64_t since_ms) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT COUNT(*) FROM pending_group_invites WHERE status = 'pending' AND created_at >= ?;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare invite count");
  }
  sqlite3_bind_int64(stmt, 1, since_ms);
  size_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return count;
}

Roe<void> GroupRosterStore::UpsertGroupTarget(const std::string& group_id, const std::string& local_thread_id,
                                              const uint32_t session_epoch, const uint64_t next_outgoing_seq) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO group_targets (group_id, local_thread_id, session_epoch, next_outgoing_seq) VALUES (?, ?, ?, ?) "
      "ON CONFLICT(group_id) DO UPDATE SET local_thread_id=excluded.local_thread_id, "
      "session_epoch=excluded.session_epoch, next_outgoing_seq=excluded.next_outgoing_seq;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group target upsert");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, local_thread_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, static_cast<int>(session_epoch));
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(next_outgoing_seq));
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Error("Failed to upsert group target");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return {};
}

Roe<void> GroupRosterStore::ClearGroupTarget(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM group_targets WHERE group_id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group target delete");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return {};
}

Roe<void> GroupRosterStore::ClearGroupTargetByThreadId(const std::string& local_thread_id) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "DELETE FROM group_targets WHERE local_thread_id = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group target delete by thread");
  }
  sqlite3_bind_text(stmt, 1, local_thread_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return {};
}

Roe<std::optional<std::string>> GroupRosterStore::FindThreadIdForGroup(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT local_thread_id FROM group_targets WHERE group_id = ? LIMIT 1;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group target lookup");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<std::string> thread_id;
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0)) {
    thread_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return Roe<std::optional<std::string>>(thread_id);
}

Roe<uint64_t> GroupRosterStore::AllocateGroupSenderSeq(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT next_outgoing_seq FROM group_targets WHERE group_id = ? LIMIT 1;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group seq lookup");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Error("No group target for group_id");
  }
  const uint64_t seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  sqlite3_finalize(stmt);
  if (sqlite3_prepare_v2(db, "UPDATE group_targets SET next_outgoing_seq = next_outgoing_seq + 1 WHERE group_id = ?;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group seq bump");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return seq;
}

Roe<uint64_t> GroupRosterStore::AllocateMemberSeq(const std::string& group_id, const std::string& member_identity,
                                                  const uint32_t pair_epoch, const std::string& member_device) const {
  // A device pair counts on its own: its row is the member's with the device (M021).
  const std::string counter = member_device.empty() ? member_identity : member_identity + "/" + member_device;
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  // One statement: a new pair epoch starts the count at 1; the same one continues it. (An older epoch than
  // the one held never comes — pair epochs only rise — and would continue the count, never repeat it.)
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO group_member_seq (group_id, member_identity, session_epoch, next_seq) VALUES (?, ?, ?, 2) "
      "ON CONFLICT(group_id, member_identity) DO UPDATE SET "
      "next_seq = CASE WHEN excluded.session_epoch > session_epoch THEN 2 ELSE next_seq + 1 END, "
      "session_epoch = MAX(session_epoch, excluded.session_epoch) "
      "RETURNING next_seq - 1;";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group member seq");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, counter.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(pair_epoch));
  std::optional<uint64_t> seq;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (!seq) {
    return Error("Failed to allocate group member seq");
  }
  return *seq;
}

Roe<uint32_t> GroupRosterStore::GetGroupSessionEpoch(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT session_epoch FROM group_targets WHERE group_id = ? LIMIT 1;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group epoch lookup");
  }
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Error("No group target");
  }
  const uint32_t epoch = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return epoch;
}

Roe<void> GroupRosterStore::BumpGroupSessionEpoch(const std::string& group_id, const uint32_t new_epoch) const {
  sqlite3* db = nullptr;
  if (!OpenDb(&db)) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db, "UPDATE group_targets SET session_epoch = ?, next_outgoing_seq = 1 WHERE group_id = ?;", -1, &stmt,
          nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to prepare group epoch bump");
  }
  sqlite3_bind_int(stmt, 1, static_cast<int>(new_epoch));
  sqlite3_bind_text(stmt, 2, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return {};
}

} // namespace pbr
