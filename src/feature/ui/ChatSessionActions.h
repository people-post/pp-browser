#pragma once

#include <functional>
#include <string>

namespace pbr {

struct ChatSessionActions {
  std::function<void()> reload_agent_config;
  std::function<void()> finalize_thread_display;
  std::function<void(const std::string& thread_id)> select_thread;
  std::function<void()> on_find_someone;
  std::function<void()> on_profile_data_reset;

  static ChatSessionActions& Instance();
};

} // namespace pbr
