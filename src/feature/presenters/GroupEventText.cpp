#include "feature/presenters/GroupEventText.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupMembershipCodec.h"

namespace pbr {
namespace {

/** `{name}` and `{title}` in `templ` replaced in one pass, so a value that holds the other marker stays as it is. */
std::string FillOnce(const std::string& templ, const std::string& name, const std::string& title) {
  std::string out;
  for (size_t i = 0; i < templ.size();) {
    if (templ.compare(i, 6, "{name}") == 0) {
      out += name;
      i += 6;
    } else if (templ.compare(i, 7, "{title}") == 0) {
      out += title;
      i += 7;
    } else {
      out.push_back(templ[i++]);
    }
  }
  return out;
}

} // namespace

std::optional<std::string> GroupEventText(const ThreadMessage& message,
                                          const std::function<std::string(const std::string& account_id)>& member_name,
                                          const std::string& local_account) {
  const auto type = GroupMembershipCodec::ControlTypeFromMessage(message);
  if (!type || !member_name) {
    return std::nullopt;
  }
  const auto is_self = [&local_account](const std::string& account) {
    return !account.empty() && account == local_account;
  };
  // `key` for someone else (their name in it), `you_key` when it is this account (none: same line for everyone).
  const auto line = [&](const char* key, const char* you_key, const std::string& account,
                        const std::string& title = {}) -> std::optional<std::string> {
    if (account.empty()) {
      return std::nullopt;
    }
    if (you_key && is_self(account)) {
      return FillOnce(Tr(you_key), "", title);
    }
    return FillOnce(Tr(key), member_name(account), title);
  };
  switch (*type) {
  case GroupMembershipControlType::MemberJoined: {
    auto joined = GroupMembershipCodec::DecodeMemberJoinedFromMessage(message);
    if (!joined) {
      return std::nullopt;
    }
    if (joined->role == MemberRole::Owner && is_self(joined->member_identity)) {
      return Tr("chat.group_event.created");
    }
    return line("chat.group_event.joined", "chat.group_event.you_joined", joined->member_identity);
  }
  case GroupMembershipControlType::MemberRemoved: {
    auto removed = GroupMembershipCodec::DecodeMemberRemovedFromMessage(message);
    return removed ? line("chat.group_event.removed", "chat.group_event.you_removed", removed->member_identity)
                   : std::nullopt;
  }
  case GroupMembershipControlType::MemberLeft: {
    auto left = GroupMembershipCodec::DecodeMemberLeftFromMessage(message);
    return left ? line("chat.group_event.left", "chat.group_event.you_left", left->member_identity) : std::nullopt;
  }
  case GroupMembershipControlType::GroupRenamed: {
    auto renamed = GroupMembershipCodec::DecodeGroupRenamedFromMessage(message);
    return renamed ? line("chat.group_event.renamed", "chat.group_event.you_renamed", message.sender_contact_id,
                          renamed->title)
                   : std::nullopt;
  }
  case GroupMembershipControlType::OwnerTransferred: {
    auto transferred = GroupMembershipCodec::DecodeOwnerTransferredFromMessage(message);
    if (!transferred) {
      return std::nullopt;
    }
    return transferred->leave_previous
               ? line("chat.group_event.owner_left", "chat.group_event.you_owner_left", transferred->new_owner_identity)
               : line("chat.group_event.owner", "chat.group_event.you_owner", transferred->new_owner_identity);
  }
  case GroupMembershipControlType::MemberRoleChanged: {
    auto changed = GroupMembershipCodec::DecodeMemberRoleChangedFromMessage(message);
    if (!changed) {
      return std::nullopt;
    }
    return changed->role == MemberRole::Admin
               ? line("chat.group_event.made_admin", "chat.group_event.you_made_admin", changed->member_identity)
               : line("chat.group_event.admin_removed", "chat.group_event.you_admin_removed", changed->member_identity);
  }
  case GroupMembershipControlType::GroupPresetChanged: {
    auto changed = GroupMembershipCodec::DecodeGroupPresetChangedFromMessage(message);
    if (!changed) {
      return std::nullopt;
    }
    return Tr(changed->preset == GroupPreset::Managed ? "chat.group_event.preset_managed"
                                                      : "chat.group_event.preset_open");
  }
  case GroupMembershipControlType::GroupInviteDecline:
    return line("chat.group_event.declined", nullptr, message.sender_contact_id);
  case GroupMembershipControlType::GroupDissolved: {
    // The sender is the owner; same words as the transcript line.
    if (message.sender_contact_id == kLocalSelfContactId || is_self(message.sender_contact_id)) {
      return Tr("chat.group.dissolved_by_you");
    }
    const std::string name = message.sender_contact_id.empty() ? std::string() : member_name(message.sender_contact_id);
    return name.empty() ? Tr("chat.group.dissolved_line_unnamed") : Tr("chat.group.dissolved_line", {{"name", name}});
  }
  case GroupMembershipControlType::GroupInviteRefused:
    return Tr("chat.group_event.full");
  case GroupMembershipControlType::GroupForked:
    // Both threads hold the event; the stored text says which side this is.
    if (message.text == "Forked from group") {
      return Tr("chat.group_event.forked_from");
    }
    return line("chat.group_event.forked", "chat.group_event.you_forked", message.sender_contact_id);
  default:
    return std::nullopt;
  }
}

bool IsStoredGroupEventText(const std::string& preview) {
  static const char* const kExact[] = {"Member joined the group",
                                       "You joined the group",
                                       "Owner created the group",
                                       "Member removed",
                                       "Member left",
                                       "Group ownership transferred",
                                       "Ownership transferred; previous owner left",
                                       "The group is full: you were not added",
                                       "Group forked",
                                       "Forked from group",
                                       "Invitation declined",
                                       "Group dissolved",
                                       "Member made admin",
                                       "Member no longer admin",
                                       "Group set to managed",
                                       "Group set to open"};
  for (const char* text : kExact) {
    if (preview == text) {
      return true;
    }
  }
  return preview.rfind("Group renamed to ", 0) == 0;
}

} // namespace pbr
