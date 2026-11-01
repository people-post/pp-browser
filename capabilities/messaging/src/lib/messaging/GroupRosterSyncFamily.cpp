#include "lib/messaging/GroupRosterSyncFamily.h"

#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterSync.h"

#include "common/Logger.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char kGroupRosterSyncLogger[] = "GroupRosterSync";

std::optional<GroupMembershipControlType> LaneTypeOf(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System) {
    return std::nullopt;
  }
  const auto kind = GroupMembershipCodec::ControlTypeFromMessage(message);
  if (kind != GroupMembershipControlType::GroupRosterRequest && kind != GroupMembershipControlType::GroupRosterReply &&
      kind != GroupMembershipControlType::GroupDissolvedSibling) {
    return std::nullopt;
  }
  return kind;
}

Roe<void> ApplyRequest(GroupRosterStore& roster, const std::string& local_account, const std::string& detail,
                       const std::string& asker,
                       const std::function<void(group_roster_sync_codec::RosterRequest, std::string)>& answer) {
  auto request = group_roster_sync_codec::DecodeRequest(detail);
  if (!request) {
    return request.error();
  }
  auto member = roster.IsMember(request->group_id, local_account);
  if (!member) {
    return member.error();
  }
  const bool answering = *member && asker != local_account && answer;
  logging::getLogger(kGroupRosterSyncLogger).info << "Roster request group_id=" << request->group_id << " from=" << asker
             << (answering ? " (answering)" : " (not a member here: no answer)");
  if (answering) {
    answer(std::move(*request), asker);
  }
  return {};
}

using DissolvedFn = std::function<void(GroupDissolvedEnd)>;

Roe<void> ApplyReply(GroupRosterStore& roster, const std::string& local_account, const std::string& detail,
                     const std::string& replier, const DissolvedFn& on_dissolved) {
  // A reply may reach a member the replier says was removed: no membership check here.
  auto reply = group_roster_sync_codec::DecodeReply(detail);
  if (!reply) {
    return reply.error();
  }
  GroupDissolvedApplied dissolved;
  auto applied = group_roster_sync::ApplyReply(roster, *reply, local_account, replier, &dissolved);
  if (!applied) {
    return applied.error();
  }
  if (*applied == group_roster_sync::ReplyOutcome::Dissolved && on_dissolved && reply->statement) {
    on_dissolved({reply->group_id, reply->statement->owner_identity, std::move(dissolved.voided_invites)});
  }
  static constexpr const char* kOutcome[] = {"ignored", "active", "removed", "dissolved"};
  logging::getLogger(kGroupRosterSyncLogger).info << "Roster reply group_id=" << reply->group_id << " from=" << replier
             << " statement=" << (reply->statement ? reply->statement->roster_epoch : 0)
             << " removed=" << reply->removed << " -> " << kOutcome[static_cast<int>(*applied)];
  return {};
}

/** The owner's other device says it dissolved a group (G016): the same dissolve, from our own account. */
Roe<void> ApplySiblingDissolved(GroupRosterStore& roster, const std::string& local_account, const std::string& detail,
                                const std::string& sender, const DissolvedFn& on_dissolved) {
  if (sender != local_account) {
    return {};  // only our own devices speak for our account
  }
  auto statement = group_roster_sync_codec::DecodeStatement(detail);
  if (!statement) {
    return statement.error();
  }
  auto ended = ApplyDissolvedStatement(roster, *statement, local_account);
  if (!ended) {
    return ended.error();
  }
  if (ended->applied && on_dissolved) {
    on_dissolved({statement->group_id, statement->owner_identity, std::move(ended->voided_invites)});
  }
  return {};
}

} // namespace

SystemControlFamily MakeGroupRosterSyncFamily(
    GroupRosterStore& roster, std::function<std::optional<std::string>()> local_account,
    std::function<void(group_roster_sync_codec::RosterRequest request, std::string asker)> answer,
    std::function<void(GroupDissolvedEnd end)> on_dissolved) {
  SystemControlFamily family;
  family.name = kGroupRosterSyncFamily;
  family.owns = [](const ThreadMessage& message) { return LaneTypeOf(message).has_value(); };
  family.on_lane = [](const ThreadMessage& message) { return LaneTypeOf(message).has_value(); };
  family.apply = [&roster, local_account = std::move(local_account), answer = std::move(answer),
                  on_dissolved = std::move(on_dissolved)](
                     ThreadMessage& message, const SystemControlContext& context) -> Roe<void> {
    const auto kind = LaneTypeOf(message);
    const std::optional<std::string> local = local_account ? local_account() : std::nullopt;
    if (!kind || !local || context.sender_identity.empty()) {
      return {};
    }
    auto detail = group_roster_sync_codec::DetailOf(message);
    if (!detail) {
      return Error("roster sync: no detail");
    }
    if (*kind == GroupMembershipControlType::GroupRosterRequest) {
      return ApplyRequest(roster, *local, *detail, context.sender_identity, answer);
    }
    if (*kind == GroupMembershipControlType::GroupDissolvedSibling) {
      return ApplySiblingDissolved(roster, *local, *detail, context.sender_identity, on_dissolved);
    }
    return ApplyReply(roster, *local, *detail, context.sender_identity, on_dissolved);
  };
  return family;
}

} // namespace pbr
