#include "gui/chat/ActionNoticeText.h"

#include "foundation/i18n/LocalizationService.h"

namespace pbr {

std::string ActionNoticeText(const ActionNotice& notice) {
  const auto named = [&notice](const char* key) { return Tr(key, {{"name", notice.name}}); };
  switch (notice.kind) {
  case ActionNotice::Kind::OpenedConversation:
    return named("chat.action.opened_conversation");
  case ActionNotice::Kind::OpenedSecureConversation:
    return named("chat.action.opened_secure_conversation");
  case ActionNotice::Kind::AddedContact:
    return named("chat.action.added_contact");
  case ActionNotice::Kind::OpenedThread:
    return named("chat.action.opened_thread");
  case ActionNotice::Kind::Registered:
    return Tr("chat.action.registered");
  case ActionNotice::Kind::RegistrationFailed:
    return Tr("chat.action.registration_failed");
  case ActionNotice::Kind::JoinedGroup:
    return named("chat.action.joined_group");
  case ActionNotice::Kind::DeclinedInvitation:
    return Tr("chat.action.declined_invitation");
  case ActionNotice::Kind::BlockedInviter:
    return Tr("chat.action.blocked_inviter");
  case ActionNotice::Kind::BlockedUnknownInviter:
    return Tr("chat.action.blocked_unknown_inviter");
  case ActionNotice::Kind::CreatedGroup:
    return named("chat.action.created_group");
  case ActionNotice::Kind::StartedFresh:
    return named("chat.action.started_fresh");
  case ActionNotice::Kind::OpenedOwnerChat:
    return Tr("chat.action.opened_owner_chat");
  }
  return {};
}

} // namespace pbr
