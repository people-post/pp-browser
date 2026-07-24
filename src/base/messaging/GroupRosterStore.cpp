#include "base/messaging/GroupRosterStore.h"

#include "base/messaging/GroupMembershipCodec.h"

#include "common/Utilities.h"

#include <sqlite3.h>

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

CREATE TABLE IF NOT EXISTS group_metadata (
  group_id TEXT PRIMARY KEY,
  owner_identity TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  roster_epoch INTEGER NOT NULL DEFAULT 1,
  policy_json TEXT NOT NULL DEFAULT '{}'
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
  created_at INTEGER NOT NULL
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
  return {};
}

Roe<void> GroupRosterStore::UpsertMetadata(const GroupMetadata& metadata) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db for group metadata");
  }
  if (auto schema = EnsureSchema(db); !schema) {
    sqlite3_close(db);
    return schema.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO group_metadata (group_id, owner_identity, title, roster_epoch, policy_json) "
      "VALUES (?, ?, ?, ?, ?) "
      "ON CONFLICT(group_id) DO UPDATE SET owner_identity=excluded.owner_identity, title=excluded.title, "
      "roster_epoch=excluded.roster_epoch, policy_json=excluded.policy_json;";
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
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return Error("Failed to upsert group metadata");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return {};
}

Roe<std::optional<GroupMetadata>> GroupRosterStore::LoadMetadata(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT owner_identity, title, roster_epoch, policy_json FROM group_metadata WHERE "
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
    if (auto epoch = GetGroupSessionEpoch(group_id)) {
      metadata.session_epoch = *epoch;
    }
    result = std::move(metadata);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return result;
}

Roe<void> GroupRosterStore::UpsertMember(const std::string& group_id, const GroupRosterMember& member) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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
  return {};
}

Roe<void> GroupRosterStore::RemoveMember(const std::string& group_id, const std::string& member_identity) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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
  return {};
}

Roe<std::vector<GroupRosterMember>> GroupRosterStore::ListMembers(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO pending_group_invites (invite_nonce, group_id, group_title, inviter_identity, invitee_identity, "
      "roster_epoch, status, expires_at, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(invite_nonce) DO UPDATE SET status=excluded.status, group_title=excluded.group_title, "
      "roster_epoch=excluded.roster_epoch;";
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
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db");
  }
  (void)EnsureSchema(db);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT group_id, group_title, inviter_identity, invitee_identity, roster_epoch, status, "
                         "expires_at, created_at FROM pending_group_invites WHERE invite_nonce = ? LIMIT 1;",
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
    result = std::move(invite);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return result;
}

Roe<void> GroupRosterStore::UpdateInviteStatus(const std::string& invite_nonce, const InviteStatus status) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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

Roe<size_t> GroupRosterStore::CountPendingInvitesSince(const int64_t since_ms) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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

Roe<std::optional<std::string>> GroupRosterStore::FindThreadIdForGroup(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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

Roe<uint32_t> GroupRosterStore::GetGroupSessionEpoch(const std::string& group_id) const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
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
