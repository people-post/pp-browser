#pragma once

#include "feature/settings/UpdatePrefs.h"
#include "gui/RecoveryReminder.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pbr {

/** What the Chats page's "new version" card needs from client-compat: the newest version and where to get it. */
struct UpdateAdvice {
  /** The document's latest_client_version (the card's "which announcement" key). */
  std::string latest_version;
  /** The usable update link (UsableUpdateLink); empty: the card has no Update button. */
  std::string url;
};

/** What ChatController needs to draw and answer the card; Application wires it to ClientCompatController. */
struct UpdateCardPorts {
  /** The current advice (ClientCompatController::SoftUpdate). */
  std::function<std::optional<UpdateAdvice>()> advice;
  /** The directory of machine.json (RunLogPrefs::MachineDir). */
  std::function<std::string()> machine_dir;
  /** Unix ms; unset: the real clock. */
  std::function<int64_t()> now_ms;
  /** The Update button: opens the link in the system. */
  std::function<void(const std::string& url)> open_url;
};

/**
 * The card is up while a newer version exists; "Later" hides it and, on the recovery card's rhythm
 * (kRecoveryReminderRepeatMs, 7 days), it returns once, then never for that version. A still-newer version starts over.
 */
inline bool ShouldShowUpdateCard(const std::string& latest_version, const UpdateCardState& state,
                                 const int64_t now_ms) {
  if (latest_version.empty()) {
    return false;
  }
  if (state.version != latest_version || state.dismissals <= 0) {
    return true;
  }
  return state.dismissals == 1 && now_ms - state.dismissed_ms >= kRecoveryReminderRepeatMs;
}

/** The state after "Later" on `latest_version` at `now_ms`. */
inline UpdateCardState UpdateCardAfterLater(const std::string& latest_version, const UpdateCardState& state,
                                            const int64_t now_ms) {
  UpdateCardState next;
  next.version = latest_version;
  next.dismissals = state.version == latest_version ? (state.dismissals >= 2 ? 2 : state.dismissals + 1) : 1;
  next.dismissed_ms = now_ms;
  return next;
}

} // namespace pbr
