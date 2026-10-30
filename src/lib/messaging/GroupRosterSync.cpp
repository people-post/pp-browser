#include "lib/messaging/GroupRosterSync.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <set>
#include "common/PbrCompat.h"

namespace pbr {
namespace group_roster_sync {
namespace {

Roe<GroupMetadata> MetadataOf(GroupRosterStore& roster, const std::string& group_id) {
  auto metadata = roster.LoadMetadata(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!metadata->has_value()) {
    return Error("Group metadata not found: " + group_id);
  }
  return **metadata;
}

std::string ToJsonText(const GroupRosterStatement& statement) {
  return DumpJson(group_roster_statement::ToJson(statement));
}

/** The statement among held and `candidate` that wins (candidate is valid; held was when kept). */
bool CandidateWins(const std::optional<GroupRosterStatement>& held, const GroupRosterStatement& candidate) {
  if (!held) {
    return true;
  }
  auto picked = group_roster_statement::Pick({*held, candidate});
  return picked && picked->owner_signature_b64 == candidate.owner_signature_b64 &&
         picked->roster_epoch == candidate.roster_epoch;
}

} // namespace

std::optional<GroupRosterStatement> Held(const GroupMetadata& metadata) {
  if (metadata.roster_statement_json.empty()) {
    return std::nullopt;
  }
  auto json = TryParseObject(metadata.roster_statement_json);
  if (!json) {
    return std::nullopt;
  }
  auto statement = group_roster_statement::FromJson(*json);
  if (!statement) {
    return std::nullopt;
  }
  return std::move(*statement);
}

Roe<bool> KeepStatement(GroupRosterStore& roster, const GroupRosterStatement& statement) {
  auto lock = GroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, statement.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (auto valid = group_roster_statement::Verify(statement, metadata->owner_identity); !valid) {
    return valid.error();
  }
  if (!CandidateWins(Held(*metadata), statement)) {
    return false;
  }
  metadata->roster_statement_json = ToJsonText(statement);
  if (auto saved = roster.UpsertMetadata(*metadata); !saved) {
    return saved.error();
  }
  return true;
}

Roe<void> Adopt(GroupRosterStore& roster, const GroupRosterStatement& statement) {
  auto lock = GroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, statement.group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto current = roster.ListMembers(statement.group_id);
  if (!current) {
    return current.error();
  }
  std::set<std::string> stated;
  for (const GroupRosterStatementMember& m : statement.members) {
    stated.insert(m.identity);
  }
  for (const GroupRosterMember& m : *current) {
    if (!stated.contains(m.member_identity)) {
      if (auto removed = roster.RemoveMember(statement.group_id, m.member_identity); !removed) {
        return removed.error();
      }
    }
  }
  for (const GroupRosterStatementMember& m : statement.members) {
    GroupRosterMember member;
    member.member_identity = m.identity;
    member.role = m.role;
    member.joined_at = m.joined_at;
    for (const GroupRosterMember& had : *current) {
      if (had.member_identity == m.identity) {
        member.contact_id = had.contact_id;
      }
    }
    if (auto upserted = roster.UpsertMember(statement.group_id, member); !upserted) {
      return upserted.error();
    }
  }
  metadata->owner_identity = statement.owner_identity;
  metadata->roster_epoch = statement.roster_epoch;
  metadata->roster_statement_json = ToJsonText(statement);
  return roster.UpsertMetadata(*metadata);
}

Roe<bool> KeepAndCatchUp(GroupRosterStore& roster, const GroupRosterStatement& statement) {
  auto lock = GroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, statement.group_id);
  if (!metadata) {
    return metadata.error();
  }
  const uint64_t our_epoch = metadata->roster_epoch;
  auto kept = KeepStatement(roster, statement);
  if (!kept || !*kept) {
    return kept;
  }
  if (statement.roster_epoch > our_epoch) {
    if (auto adopted = Adopt(roster, statement); !adopted) {
      return adopted.error();
    }
  }
  return true;
}

Roe<Check> BeginCheck(GroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                      const int64_t now_ms) {
  auto lock = GroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto members = roster.ListMembers(group_id);
  if (!members) {
    return members.error();
  }
  Check check;
  check.request.group_id = group_id;
  check.request.request_id = util::GenerateUuid();
  check.request.roster_epoch = metadata->roster_epoch;
  std::set<std::string> ask;
  if (!metadata->owner_identity.empty()) {
    ask.insert(metadata->owner_identity);
  }
  for (const GroupRosterMember& m : *members) {
    ask.insert(m.member_identity);
  }
  ask.erase(local_identity);
  check.ask.assign(ask.begin(), ask.end());

  if (metadata->sync_state != GroupSyncState::Removed) {
    metadata->sync_state = GroupSyncState::Checking;
  }
  metadata->sync_request_id = check.request.request_id;
  metadata->sync_requested_at = now_ms;
  if (auto saved = roster.UpsertMetadata(*metadata); !saved) {
    return saved.error();
  }
  return check;
}

Roe<std::optional<group_roster_sync_codec::RosterReply>> Answer(GroupRosterStore& roster,
                                                                const group_roster_sync_codec::RosterRequest& request,
                                                                const std::string& asker) {
  auto metadata = roster.LoadMetadata(request.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!metadata->has_value()) {
    return std::optional<group_roster_sync_codec::RosterReply>{};
  }
  group_roster_sync_codec::RosterReply reply;
  reply.group_id = request.group_id;
  reply.request_id = request.request_id;
  reply.statement = Held(**metadata);
  if ((*metadata)->sync_state == GroupSyncState::Active) {
    auto member = roster.IsMember(request.group_id, asker);
    if (!member) {
      return member.error();
    }
    reply.removed = !*member;
  }
  return std::optional<group_roster_sync_codec::RosterReply>(std::move(reply));
}

Roe<ReplyOutcome> ApplyReply(GroupRosterStore& roster, const group_roster_sync_codec::RosterReply& reply,
                             const std::string& local_identity, const std::string& replier) {
  auto lock = GroupRosterStore::LockForUpdate();
  auto metadata = MetadataOf(roster, reply.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->sync_request_id.empty() || reply.request_id != metadata->sync_request_id) {
    return ReplyOutcome::Ignored;
  }
  const auto settle = [&](GroupSyncState state) -> Roe<ReplyOutcome> {
    auto now = MetadataOf(roster, reply.group_id);
    if (!now) {
      return now.error();
    }
    now->sync_state = state;
    if (auto saved = roster.UpsertMetadata(*now); !saved) {
      return saved.error();
    }
    return state == GroupSyncState::Removed ? ReplyOutcome::Removed : ReplyOutcome::Active;
  };
  if (!reply.statement) {
    // Without a statement only the owner can tell us we are out.
    if (reply.removed && replier == metadata->owner_identity) {
      return settle(GroupSyncState::Removed);
    }
    return ReplyOutcome::Ignored;
  }
  const GroupRosterStatement& statement = *reply.statement;
  if (auto valid = group_roster_statement::Verify(statement, metadata->owner_identity); !valid) {
    return ReplyOutcome::Ignored;
  }
  if (CandidateWins(Held(*metadata), statement) && statement.roster_epoch >= metadata->roster_epoch) {
    if (auto adopted = Adopt(roster, statement); !adopted) {
      return adopted.error();
    }
  } else if (auto kept = KeepStatement(roster, statement); !kept) {
    return kept.error();
  }
  auto after = MetadataOf(roster, reply.group_id);
  if (!after) {
    return after.error();
  }
  const std::optional<GroupRosterStatement> held = Held(*after);
  const bool listed = held ? held->Lists(local_identity) : true;
  return settle(listed ? GroupSyncState::Active : GroupSyncState::Removed);
}

bool MaySend(const GroupMetadata& metadata) {
  return metadata.sync_state == GroupSyncState::Active;
}

} // namespace group_roster_sync
} // namespace pbr
