#pragma once

#include "common/Error.h"

#include <functional>
#include <string>

namespace pbr {

struct ChatSessionActions {
  std::function<void()> reload_agent_config;
  std::function<void()> finalize_thread_display;
  std::function<void(const std::string& thread_id)> select_thread;
  std::function<void()> on_find_someone;
  std::function<void()> on_profile_data_reset;
  /** App-registered: wipe profile data and reinit hub/secrets. Settings must not own that lifecycle. */
  std::function<Roe<void>()> reset_active_profile;

  static ChatSessionActions& Instance();
};

} // namespace pbr
