#pragma once

#include "lib/storage/SqliteChatStoreGate.h"

#include <functional>
#include <optional>
#include <string>

namespace pbr {

/**
 * The start-up recovery for a chat store that did not open after unlock (D105): what it says, as i18n keys, and its
 * one action, Reset chats… (it leads to the reset's own confirm). A store that opened — created, current, migrated,
 * or reset below its floor (that one is a notice) — needs none.
 */
struct ChatStoreRecoveryPrompt {
  std::string title_key;
  std::string message_key;
  std::string action_key = "settings.storage.reset_chats";
};

std::optional<ChatStoreRecoveryPrompt> ChatStoreRecoveryFor(const std::optional<ChatStoreProblem>& problem);

/**
 * Show `prompt`; its action confirms the reset (chats on this device go; the account, contacts and settings stay)
 * and then runs `reset_chats`, after which start-up goes on. Declined (either dialog), `on_declined` runs: the app
 * stays without its chats until the next unlock attempt asks again.
 */
void ShowChatStoreRecovery(const ChatStoreRecoveryPrompt& prompt, std::function<void()> reset_chats,
                           std::function<void()> on_declined);

} // namespace pbr
