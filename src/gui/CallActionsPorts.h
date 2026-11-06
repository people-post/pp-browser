#pragma once

#include "lib/calls/CallSessionQueries.h"

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
  std::function<bool(const std::string& thread_id, bool video_allowed)> start_call;
  std::function<void()> refresh_pending_ring;
  /** Platform can encode/send video for a call (gates the "Video call" menu item). */
  std::function<bool()> video_call_available;
  /** A group call goes on in this chat that this device may join (the call button reads "Join"). */
  std::function<bool(const std::string& thread_id)> call_joinable;
  /** Phones: the bar under a group chat's header while its call may be joined ("" = no bar). */
  std::function<std::string(const std::string& thread_id)> call_join_bar_text;
  /**
   * Phone Home's Calls widget: called on the UI thread, it returns a read of this profile's newest calls (at most
   * `limit`) that is safe to run on a worker (it opens its own store handle); empty while there is no profile.
   */
  std::function<std::function<std::vector<call_queries::RecentCall>()>(size_t limit)> recent_calls_reader;

  // PeoplePicker
  std::function<void(const std::vector<std::string>& identities)> invite_identities;
  std::function<bool(const std::string& thread_id, bool video_allowed,
                     const std::vector<std::string>& identities)>
      start_with_invitees;

  // Shell
  std::function<void()> accept_incoming;
  std::function<void()> accept_incoming_voice_only;
  std::function<void()> accept_incoming_with_charge;
  std::function<void()> decline_incoming;
  std::function<void()> leave_active;
  std::function<void()> retry_connect;
  std::function<void()> toggle_mute;
  std::function<void()> toggle_camera;
  std::function<void()> flip_camera;
  std::function<void()> toggle_speaker;
  std::function<void()> open_mid_call_invite_picker;
  /** G022: "Make a group…"; the group banner's Join and Not now. */
  std::function<void()> make_group_from_call;
  std::function<void()> join_call_group_invite;
  std::function<void()> put_off_call_group_invite;
  std::function<void()> minimize_chrome;
  std::function<void()> expand_chrome;
  std::function<void()> immersive_chrome;
  /** V059: Immersive's spotlight — a roster row's stream_id; back to the grid; toggle fill / fit. */
  std::function<void(const std::string&)> spotlight_participant;
  std::function<void()> spotlight_grid;
  std::function<void()> spotlight_toggle_fill;
  std::function<void()> restore_chrome_from_minimized;
  std::function<void(int corner)> set_minimized_corner;
  std::function<void()> show_call_details;
};

} // namespace pbr
