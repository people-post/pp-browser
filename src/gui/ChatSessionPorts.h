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
};

} // namespace pbr
