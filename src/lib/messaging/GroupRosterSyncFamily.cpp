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
  if (kind != GroupMembershipControlType::GroupRosterRequest && kind != GroupMembershipControlType::GroupRosterReply) {
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

Roe<void> ApplyReply(GroupRosterStore& roster, const std::string& local_account, const std::string& detail,
                     const std::string& replier) {
  // A reply may reach a member the replier says was removed: no membership check here.
  auto reply = group_roster_sync_codec::DecodeReply(detail);
  if (!reply) {
    return reply.error();
  }
  auto applied = group_roster_sync::ApplyReply(roster, *reply, local_account, replier);
  if (!applied) {
    return applied.error();
  }
  static constexpr const char* kOutcome[] = {"ignored", "active", "removed"};
  logging::getLogger(kGroupRosterSyncLogger).info << "Roster reply group_id=" << reply->group_id << " from=" << replier
             << " statement=" << (reply->statement ? reply->statement->roster_epoch : 0)
             << " removed=" << reply->removed << " -> " << kOutcome[static_cast<int>(*applied)];
  return {};
}

} // namespace

SystemControlFamily MakeGroupRosterSyncFamily(
    GroupRosterStore& roster, std::function<std::optional<std::string>()> local_account,
    std::function<void(group_roster_sync_codec::RosterRequest request, std::string asker)> answer) {
  SystemControlFamily family;
  family.name = kGroupRosterSyncFamily;
  family.owns = [](const ThreadMessage& message) { return LaneTypeOf(message).has_value(); };
  family.on_lane = [](const ThreadMessage& message) { return LaneTypeOf(message).has_value(); };
  family.apply = [&roster, local_account = std::move(local_account), answer = std::move(answer)](
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
    return ApplyReply(roster, *local, *detail, context.sender_identity);
  };
  return family;
}

} // namespace pbr
