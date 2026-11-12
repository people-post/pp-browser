#include "gui/ChatStoreRecovery.h"

#include "foundation/i18n/LocalizationService.h"
#include "gui/UserFeedback.h"

#include <utility>

namespace pbr {

std::optional<ChatStoreRecoveryPrompt> ChatStoreRecoveryFor(const std::optional<ChatStoreProblem>& problem) {
  if (!problem) {
    return std::nullopt;
  }
  ChatStoreRecoveryPrompt prompt;
  if (*problem == ChatStoreProblem::Newer) {
    prompt.title_key = "chats.recovery.newer_title";
    prompt.message_key = "chats.recovery.newer_message";
  } else {
    prompt.title_key = "chats.recovery.not_upgraded_title";
    prompt.message_key = "chats.recovery.not_upgraded_message";
  }
  return prompt;
}

void ShowChatStoreRecovery(const ChatStoreRecoveryPrompt& prompt, std::function<void()> reset_chats,
                           std::function<void()> on_declined) {
  UserFeedback::Confirm(
      Tr(prompt.title_key), Tr(prompt.message_key),
      [reset_chats = std::move(reset_chats), on_declined = std::move(on_declined)](const bool reset) mutable {
        if (!reset) {
          if (on_declined) {
            on_declined();
          }
          return;
        }
        UserFeedback::ConfirmDestructive(
            Tr("settings.storage.reset_chats_confirm_title"), Tr("settings.storage.reset_chats_confirm_message"),
            [reset_chats = std::move(reset_chats), on_declined = std::move(on_declined)](const bool confirmed) {
              if (confirmed) {
                if (reset_chats) {
                  reset_chats();
                }
              } else if (on_declined) {
                on_declined();
              }
            },
            Tr("settings.storage.reset_chats_confirm_ok"));
      },
      Tr(prompt.action_key));
}

} // namespace pbr
