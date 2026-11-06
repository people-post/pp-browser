#include "lib/messaging/GroupLinkJoin.h"

#include "lib/messaging/GroupPermissions.h"
#include "lib/messaging/GroupRosterSync.h"

#include "foundation/crypto/CryptoUtil.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <type_traits>
#include "common/PbrCompat.h"

namespace pbr {
namespace group_link_join {
namespace {

constexpr std::pair<GroupJoinRefusal, const char*> kRefusalWords[] = {
    {GroupJoinRefusal::Expired, "expired"},      {GroupJoinRefusal::Revoked, "revoked"},
    {GroupJoinRefusal::UsedUp, "used_up"},       {GroupJoinRefusal::Removed, "removed"},
    {GroupJoinRefusal::GroupFull, "group_full"}, {GroupJoinRefusal::Rate, "rate"},
};

const char* TypeOf(const GroupJoinMessage& message) {
  switch (message.index()) {
    case 0:
      return kGroupJoinRequestType;
    case 1:
      return kGroupJoinPreviewType;
    case 2:
      return kGroupJoinRefusedType;
    default:
      return kGroupJoinConfirmType;
  }
}

Object DetailOf(const GroupJoinMessage& message) {
  Object d;
  std::visit(
      [&d](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        d.set("request_id", m.request_id);
        if constexpr (std::is_same_v<T, GroupJoinRequest>) {
          d.set("link_id", m.link_id);
          d.set("secret", BytesToHex(m.secret));
        } else if constexpr (std::is_same_v<T, GroupJoinPreview>) {
          d.set("group_id", m.group_id);
          d.set("owner", m.owner_identity);
          d.set("group_title", m.group_title);
          d.setJsonUInt("member_count", m.member_count);
          d.set("device", m.device);
        } else if constexpr (std::is_same_v<T, GroupJoinRefused>) {
          d.set("reason", RefusalToString(m.reason));
        } else {
          d.set("device", m.device);
        }
      },
      message);
  return d;
}

Roe<GroupJoinMessage> FromDetail(const std::string& type, const Object& d) {
  const std::string request_id = d.getString("request_id").value_or("");
  if (request_id.empty()) {
    return Error("join by link: no request id");
  }
  if (type == kGroupJoinRequestType) {
    GroupJoinRequest r{.request_id = request_id, .link_id = d.getString("link_id").value_or("")};
    auto secret = HexToBytes(d.getString("secret").value_or(""));
    if (r.link_id.empty() || !secret || secret->size() != group_invite_link::kSecretBytes) {
      return Error("join by link: a malformed request");
    }
    r.secret = std::move(*secret);
    return GroupJoinMessage(std::move(r));
  }
  if (type == kGroupJoinPreviewType) {
    GroupJoinPreview p{.request_id = request_id,
                       .group_id = d.getString("group_id").value_or(""),
                       .owner_identity = d.getString("owner").value_or(""),
                       .group_title = d.getString("group_title").value_or(""),
                       .member_count = d.getNonNegInt("member_count").value_or(0),
                       .device = d.getString("device").value_or("")};
    if (p.group_id.empty() || p.owner_identity.empty() || p.device.empty()) {
      return Error("join by link: a malformed preview");
    }
    return GroupJoinMessage(std::move(p));
  }
  if (type == kGroupJoinRefusedType) {
    return GroupJoinMessage(
        GroupJoinRefused{.request_id = request_id, .reason = RefusalFromString(d.getString("reason").value_or(""))});
  }
  GroupJoinConfirm c{.request_id = request_id, .device = d.getString("device").value_or("")};
  if (c.device.empty()) {
    return Error("join by link: a confirm names no device");
  }
  return GroupJoinMessage(std::move(c));
}

std::optional<std::string> JoinTypeOf(const ThreadMessage& message, Object* payload_out = nullptr) {
  if (message.content_type != ChatContentType::System) {
    return std::nullopt;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::nullopt;
  }
  const std::string type = payload->getString("control_type").value_or("");
  if (type != kGroupJoinRequestType && type != kGroupJoinPreviewType && type != kGroupJoinRefusedType &&
      type != kGroupJoinConfirmType) {
    return std::nullopt;
  }
  if (payload_out) {
    *payload_out = std::move(*payload);
  }
  return type;
}

/** Where we stand in the link's group: in it and still allowed to bring people in. */
bool MayAdmit(const GroupRosterView& view, const std::string& local_identity) {
  const GroupRosterViewMember* self = view.Find(local_identity);
  return self && GroupAllows(GroupAction::Invite, self->role, view.preset);
}

/**
 * The group's statement or a held change names `link_id` at all (a text search, no signature checked): a stranger's
 * request for a link no group here knows costs no roster computation.
 */
bool MayName(IGroupRosterStore& roster, const std::string& group_id, const std::string& link_id) {
  if (auto metadata = roster.LoadMetadata(group_id);
      metadata && *metadata && (*metadata)->roster_statement_json.find(link_id) != std::string::npos) {
    return true;
  }
  auto held = roster.ListGroupChanges(group_id);
  return held && std::any_of(held->begin(), held->end(), [&link_id](const StoredGroupChange& change) {
           return change.change_json.find(link_id) != std::string::npos;
         });
}

} // namespace

std::string RefusalToString(const GroupJoinRefusal reason) {
  for (const auto& [r, word] : kRefusalWords) {
    if (r == reason) {
      return word;
    }
  }
  return "revoked";
}

GroupJoinRefusal RefusalFromString(const std::string& value) {
  for (const auto& [r, word] : kRefusalWords) {
    if (value == word) {
      return r;
    }
  }
  return GroupJoinRefusal::Revoked;
}

Roe<ThreadMessage> Build(const GroupJoinMessage& message) {
  Object payload;
  payload.set("control_type", TypeOf(message));
  payload.set("detail", DumpJson(DetailOf(message))); // a system payload keeps its type and one detail string
  ThreadMessage out;
  out.id = util::GenerateUuid();
  out.content_type = ChatContentType::System;
  out.text = "Group join";
  out.payload_json = DumpJson(payload);
  out.timestamp = util::NowUnixMs();
  return out;
}

Roe<std::optional<GroupJoinMessage>> Parse(const ThreadMessage& message) {
  Object payload;
  const std::optional<std::string> type = JoinTypeOf(message, &payload);
  if (!type) {
    return std::optional<GroupJoinMessage>{};
  }
  auto detail = TryParseObject(payload.getString("detail").value_or(""));
  if (!detail) {
    return Error("join by link: no detail");
  }
  auto parsed = FromDetail(*type, *detail);
  if (!parsed) {
    return parsed.error();
  }
  return std::optional<GroupJoinMessage>(std::move(*parsed));
}

bool IsJoinMessage(const ThreadMessage& message) {
  return JoinTypeOf(message).has_value();
}

namespace {

JoinJudgement Refuse(const GroupJoinRefusal reason) {
  return {.kind = JoinJudgement::Kind::Refuse, .reason = reason};
}

/** The request's secret is the link's: by the roster's hash when it names the link, else by the secret we kept. */
bool SecretChecks(const JoinCase& join, const GroupInviteLink* known) {
  if (known) {
    return group_invite_link::SecretMatches(*known, join.secret);
  }
  return join.made_here_expires_at && group_invite_link::SecretsEqual(join.made_here_secret, join.secret);
}

/** After the secret matched: why the link no longer lets anyone in, if it does not. */
std::optional<GroupJoinRefusal> LinkEnded(const JoinCase& join, const GroupEndedLink* ended, const bool known) {
  if (!known) {
    // A link of ours the roster no longer names (its revoke or its maker's leave folded away, or the group gone).
    return join.now_ms >= *join.made_here_expires_at ? GroupJoinRefusal::Expired : GroupJoinRefusal::Revoked;
  }
  if (ended) {
    return ended->end == GroupLinkEnd::Expired ? GroupJoinRefusal::Expired : GroupJoinRefusal::Revoked;
  }
  return std::nullopt;
}

} // namespace

JoinJudgement JudgeJoin(const JoinCase& join) {
  const GroupInviteLink* live = join.view ? join.view->LiveLink(join.link_id) : nullptr;
  const GroupEndedLink* ended = join.view ? join.view->EndedLink(join.link_id) : nullptr;
  const GroupInviteLink* known = live ? live : ended ? &ended->link : nullptr;
  if (!SecretChecks(join, known)) {
    return {}; // Drop: no reply tells a stranger whether the link exists
  }
  if (join.within_rate && !join.within_rate()) {
    return Refuse(GroupJoinRefusal::Rate);
  }
  if (const std::optional<GroupJoinRefusal> end = LinkEnded(join, ended, known != nullptr)) {
    return Refuse(*end);
  }
  const GroupRosterView& view = *join.view;
  if (!MayAdmit(view, join.local_identity)) {
    return Refuse(GroupJoinRefusal::Revoked);
  }
  if (live->max_uses > 0 && live->uses >= live->max_uses) {
    return Refuse(GroupJoinRefusal::UsedUp);
  }
  const bool removed = std::any_of(view.removed.begin(), view.removed.end(),
                                   [&](const GroupRemovedMember& r) { return r.identity == join.joiner; });
  if (removed) {
    return Refuse(GroupJoinRefusal::Removed);
  }
  if (!view.Lists(join.joiner) && view.members.size() >= kMaxGroupMembers) {
    return Refuse(GroupJoinRefusal::GroupFull);
  }
  return {.kind = JoinJudgement::Kind::Accept};
}

std::optional<uint64_t> DroppedJoinLog::Note(const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  ++count_;
  if (last_logged_ && now_ms - *last_logged_ < kLogGapMs) {
    return std::nullopt;
  }
  last_logged_ = now_ms;
  const uint64_t count = count_;
  count_ = 0;
  return count;
}

std::optional<LinkGroup> FindLinkGroup(IGroupRosterStore& roster, const std::string& link_id,
                                       const GroupCardLookup& cards, const int64_t now_ms) {
  auto ids = roster.ListGroupIds();
  if (!ids || link_id.empty()) {
    return std::nullopt;
  }
  for (const std::string& group_id : *ids) {
    if (!MayName(roster, group_id, link_id)) {
      continue; // the roster's checks verify every held change: only for a group whose text names the link
    }
    std::optional<GroupRosterView> view = group_roster_sync::CurrentView(roster, group_id, cards, now_ms);
    if (view && (view->LiveLink(link_id) || view->EndedLink(link_id))) {
      return LinkGroup{.group_id = group_id, .view = std::move(*view)};
    }
  }
  return std::nullopt;
}

PendingGroupInvite JoinRecord(const std::string& request_id, const std::string& group_id, const std::string& title,
                              const std::string& owner_identity, const std::string& answerer,
                              const std::string& local_identity, const int64_t now_ms) {
  return {.invite_nonce = request_id,
          .group_id = group_id,
          .group_title = title,
          .inviter_identity = answerer,
          .invitee_identity = local_identity,
          .status = InviteStatus::Accepted,
          .created_at = now_ms,
          .owner_identity = owner_identity == answerer ? std::string() : owner_identity};
}

bool JoinRequestLimiter::Allow(const std::string& link_id, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  std::deque<int64_t>& seen = seen_[link_id];
  while (!seen.empty() && seen.front() <= now_ms - kRateWindowMs) {
    seen.pop_front();
  }
  if (seen.size() >= kRequestsPerHour) {
    return false;
  }
  seen.push_back(now_ms);
  return true;
}

} // namespace group_link_join
} // namespace pbr
