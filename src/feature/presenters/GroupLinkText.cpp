#include "feature/presenters/GroupLinkText.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupPermissions.h"

namespace pbr {

bool ShowsInviteLink(const MemberRole role, const GroupPreset preset) {
  return GroupAllows(GroupAction::MakeInviteLink, role, preset);
}

bool ListsGroupLinks(const MemberRole role, const GroupPreset preset) {
  return GroupAllows(GroupAction::RevokeInviteLink, role, preset);
}

std::string GroupLinkExpiryText(const int64_t expires_at, const int64_t now_ms) {
  const int64_t left_ms = expires_at - now_ms;
  if (left_ms <= 0) {
    return Tr("group_link.expired");
  }
  constexpr int64_t kMinute = 60 * 1000;
  constexpr int64_t kHour = 60 * kMinute;
  constexpr int64_t kDay = 24 * kHour;
  // Rounded up: a link that has 6 days 23 hours left "expires in 7 days".
  const auto in = [left_ms](const int64_t unit, const std::string& key) {
    const int64_t count = (left_ms + unit - 1) / unit;
    return count == 1 ? Tr(key + "_one") : Tr(key, {{"count", std::to_string(count)}});
  };
  if (left_ms > kDay) {
    return in(kDay, "group_link.expires_days");
  }
  if (left_ms > kHour) {
    return in(kHour, "group_link.expires_hours");
  }
  return in(kMinute, "group_link.expires_minutes");
}

std::vector<GroupLinkListRow> BuildGroupLinkRows(const std::vector<GroupLinkRow>& links,
                                                 const std::function<std::string(const std::string&)>& member_name,
                                                 const std::string& local_account, const int64_t now_ms) {
  std::vector<GroupLinkListRow> rows;
  rows.reserve(links.size());
  for (const GroupLinkRow& link : links) {
    GroupLinkListRow row;
    row.link_id = link.link_id;
    row.label = link.maker == local_account
                    ? Tr("group_link.yours")
                    : Tr("group_link.someones", {{"name", member_name ? member_name(link.maker) : link.maker}});
    row.detail = GroupLinkExpiryText(link.expires_at, now_ms);
    if (link.max_uses > 0) {
      row.detail += " · " + Tr("group_link.used_of",
                               {{"count", std::to_string(link.uses)}, {"max", std::to_string(link.max_uses)}});
    } else if (link.uses > 0) {
      row.detail += " · " + (link.uses == 1 ? Tr("group_link.used_one")
                                            : Tr("group_link.used", {{"count", std::to_string(link.uses)}}));
    }
    row.can_revoke = link.can_revoke;
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string GroupJoinPreviewText(const std::string& title, const uint64_t member_count) {
  const std::string count = std::to_string(member_count);
  return Tr("group_join.preview", {{"name", title}, {"count", count}});
}

std::string GroupJoinRefusalText(const GroupJoinRefusal reason) {
  switch (reason) {
    case GroupJoinRefusal::Expired:
      return Tr("group_join.refused.expired");
    case GroupJoinRefusal::Revoked:
      return Tr("group_join.refused.revoked");
    case GroupJoinRefusal::UsedUp:
      return Tr("group_join.refused.used_up");
    case GroupJoinRefusal::Removed:
      return Tr("group_join.refused.removed");
    case GroupJoinRefusal::GroupFull:
      return Tr("group_join.refused.group_full");
    case GroupJoinRefusal::Rate:
      return Tr("group_join.refused.rate");
    case GroupJoinRefusal::Unknown:
      break;
  }
  return Tr("group_join.refused.unknown");
}

std::string GroupJoinNoAnswerText(const std::string& maker_name) {
  return Tr("group_join.no_answer", {{"name", maker_name}});
}

GroupJoinStep NextGroupJoinStep(const GroupJoinState& state, const std::string& maker_name) {
  switch (state.phase) {
    case GroupJoinState::Phase::Preview:
      if (state.preview) {
        return {.kind = GroupJoinStep::Kind::Preview,
                .title = Tr("group_join.title"),
                .text = GroupJoinPreviewText(state.preview->group_title, state.preview->member_count)};
      }
      break;
    case GroupJoinState::Phase::Refused:
      return {.kind = GroupJoinStep::Kind::Tell, .text = GroupJoinRefusalText(state.reason)};
    case GroupJoinState::Phase::NoAnswer:
      return {.kind = GroupJoinStep::Kind::Tell, .text = GroupJoinNoAnswerText(maker_name)};
    case GroupJoinState::Phase::Asking:
    case GroupJoinState::Phase::Joined:
    case GroupJoinState::Phase::Cancelled:
      break;
  }
  return {};
}

std::string GroupJoinAskFailureText(const Error& error) {
  if (error.message == "group_join.err.own_link") {
    return Tr("group_join.err.own_link");
  }
  return Tr("group_join.err.bad_link");
}

} // namespace pbr
