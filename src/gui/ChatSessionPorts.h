#pragma once

#include <functional>
#include <string>

namespace pbr {

/**
 * Chat navigation ports for contacts / people-picker. Declared in UI (consumers);
 * Application fills from ChatController. Not a singleton.
 */
struct ChatSessionPorts {
  std::function<void()> finalize_thread_display;
  std::function<void(const std::string& thread_id)> select_thread;
  std::function<void()> on_find_someone;
  /** Contacts "+" menu, "New group": opens the people picker (one pick = chat, two or more = group). */
  std::function<void()> open_new_group;
  /** Phone "New chat" page, "AI chat": a new chat with the AI (what the desktop "+" menu's first item does). */
  std::function<void()> new_ai_chat;
};

} // namespace pbr
