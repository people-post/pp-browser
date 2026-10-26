#include "gui/chat/ChatActionLabel.h"

#include "common/ValueJson.h"
#include "foundation/i18n/LocalizationService.h"

#include <array>
#include <string_view>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kLabelKeys{{
    {"accept_group_invite", "chat.group_action.accept"},
    {"decline_group_invite", "chat.group_action.decline"},
    {"block_group_inviter", "chat.group_action.block"},
    {"fork_group", "chat.group_action.fork"},
    {"message_group_owner", "chat.group_action.message_owner"},
    {"dismiss_owner_advisory", "chat.group_action.dismiss"},
}};

} // namespace

std::optional<std::string> ChatActionLabelKey(const TranscriptChatAction& action) {
  if (!action.payload) {
    return std::nullopt;
  }
  const auto payload = TryParseObject(*action.payload);
  if (!payload) {
    return std::nullopt;
  }
  const auto type = payload->getString("type");
  if (!type) {
    return std::nullopt;
  }
  for (const auto& [wire, key] : kLabelKeys) {
    if (*type == wire) {
      return std::string(key);
    }
  }
  return std::nullopt;
}

std::string ChatActionLabel(const TranscriptChatAction& action) {
  if (const auto key = ChatActionLabelKey(action)) {
    return Tr(*key);
  }
  return action.label;
}

} // namespace pbr
