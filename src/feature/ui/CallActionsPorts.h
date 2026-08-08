#pragma once

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Call action ports for chat / shell / people-picker.
 * Declared in UI (consumers); Application fills from owned CallController.
 * Clear via BindCallActions({}).
 */
struct CallActionsPorts {
  // Chat
  std::function<bool(const std::string& thread_id)> start_voice;
  std::function<bool(const std::string& thread_id)> start_video;
  std::function<void()> refresh_pending_ring;

  // PeoplePicker
  std::function<void(const std::vector<std::string>& identities)> invite_identities;
  std::function<bool(const std::string& thread_id, bool video,
                     const std::vector<std::string>& identities)>
      start_with_invitees;

  // Shell
  std::function<void()> accept_incoming;
  std::function<void()> decline_incoming;
  std::function<void()> leave_active;
  std::function<void()> retry_connect;
  std::function<void()> toggle_mute;
  std::function<void()> toggle_camera;
  std::function<void()> toggle_speaker;
  std::function<void()> open_mid_call_invite_picker;
  std::function<void()> minimize_chrome;
  std::function<void()> expand_chrome;
  std::function<void()> immersive_chrome;
  std::function<void()> restore_chrome_from_minimized;
  std::function<void(int corner)> set_minimized_corner;
  std::function<void()> show_call_details;
};

} // namespace pbr
