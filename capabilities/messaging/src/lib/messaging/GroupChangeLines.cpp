#include "lib/messaging/GroupChangeLines.h"

#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterVersion.h"

#include "common/Utilities.h"

#include "common/PbrCompat.h"

namespace pbr {
namespace group_change_lines {

std::optional<ThreadMessage> LineFor(const std::string& thread_id, const GroupChange& change, const uint64_t epoch) {
  Roe<std::string> detail = Error("no line");
  GroupMembershipControlType type = GroupMembershipControlType::MemberLeft;
  std::string text;
  switch (change.kind) {
    case GroupChangeKind::Leave:
      detail = GroupMembershipCodec::EncodeMemberLeft(change.group_id, change.signer, epoch);
      text = "Member left";
      break;
    case GroupChangeKind::Remove:
      type = GroupMembershipControlType::MemberRemoved;
      detail = GroupMembershipCodec::EncodeMemberRemoved(change.group_id, change.member, epoch, change.signer);
      text = "Member removed";
      break;
    case GroupChangeKind::Add: {
      type = GroupMembershipControlType::MemberJoined;
      GroupMembershipCodec::MemberJoinedPayload joined;
      joined.group_id = change.group_id;
      joined.member_identity = change.member;
      joined.roster_epoch = epoch;
      joined.added_by = change.signer;          // G019 5: "Alice added Carol"
      joined.by_link = !change.link_id.empty(); // G021: "Carol joined by Alice's link"
      detail = GroupMembershipCodec::EncodeMemberJoined(joined);
      text = "Member joined the group";
      break;
    }
    case GroupChangeKind::Rename:
      type = GroupMembershipControlType::GroupRenamed;
      detail = GroupMembershipCodec::EncodeGroupRenamed(change.group_id, change.title, epoch, change.title_version);
      text = "Group renamed to " + change.title;
      break;
    default:
      return std::nullopt;
  }
  if (!detail) {
    return std::nullopt;
  }
  auto line = GroupMembershipCodec::BuildSystemMessage(thread_id, type, text, *detail, change.signer);
  if (!line) {
    return std::nullopt;
  }
  return std::move(*line);
}

Roe<std::optional<std::string>> Append(IThreadStore& store, const std::string& group_id,
                                       const std::vector<GroupChange>& changes, const uint64_t epoch,
                                       const std::string& local_identity) {
  std::optional<std::string> about_us;
  if (changes.empty()) {
    return about_us;
  }
  auto thread = store.FindGroupThread(group_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return about_us; // the chat was deleted here
  }
  for (const GroupChange& change : changes) {
    auto line = LineFor((*thread)->id, change, epoch);
    if (!line) {
      continue;
    }
    if (auto appended = store.AppendMessage(*line); !appended) {
      return appended.error();
    }
    if (change.kind == GroupChangeKind::Remove && !local_identity.empty() && change.member == local_identity) {
      about_us = (*thread)->id;
    }
  }
  GroupRosterVersion::Bump(); // the composer's `@` lines see the new roster
  return about_us;
}

Roe<void> FollowTitle(IThreadStore& store, const std::string& group_id, const std::string& title) {
  auto thread = store.FindGroupThread(group_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread || title.empty() || (*thread)->title == title) {
    return {};
  }
  Thread row = **thread;
  row.title = title;
  row.updated_at = util::NowUnixMs();
  if (auto saved = store.UpsertThread(row); !saved) {
    return saved.error();
  }
  return {};
}

} // namespace group_change_lines
} // namespace pbr
