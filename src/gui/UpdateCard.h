#pragma once

#include "feature/settings/UpdatePrefs.h"
#include "gui/RecoveryReminder.h"
#include "lib/net/ClientCompat.h"

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
 * Versions are compared with CompareSemverCore (lib/net/ClientCompat.h), the comparison that decides the card itself:
 * a leading "v" is ignored and a pre-release suffix is not looked at, so "v1.2.0" and "1.2.0" are one announcement.
 * An announcement is "new" only when it is greater than the stored one (or nothing is stored); an equal or lower one
 * (a rollback of the announced version) is the stored announcement and follows its rhythm.
 */
inline bool UpdateCardIsNewAnnouncement(const std::string& latest_version, const UpdateCardState& state) {
  return state.version.empty() || CompareSemverCore(latest_version, state.version) > 0;
}

/**
 * The card is up while a newer version exists; "Later" hides it and, on the recovery card's rhythm
 * (kRecoveryReminderRepeatMs, 7 days), it returns once, then never for that announcement. A greater announced version
 * starts over. A clock that went backwards (now before the tap) counts as the 7 days having passed, so the card cannot
 * stay hidden for good (the recovery reminder has the same property, left as it is).
 */
inline bool ShouldShowUpdateCard(const std::string& latest_version, const UpdateCardState& state,
                                 const int64_t now_ms) {
  if (latest_version.empty()) {
    return false;
  }
  if (UpdateCardIsNewAnnouncement(latest_version, state) || state.dismissals <= 0) {
    return true;
  }
  return state.dismissals == 1 &&
         (now_ms < state.dismissed_ms || now_ms - state.dismissed_ms >= kRecoveryReminderRepeatMs);
}

/** The state after "Later" on `latest_version` at `now_ms`; a lower or equal announcement keeps the stored version. */
inline UpdateCardState UpdateCardAfterLater(const std::string& latest_version, const UpdateCardState& state,
                                            const int64_t now_ms) {
  UpdateCardState next;
  if (UpdateCardIsNewAnnouncement(latest_version, state)) {
    next.version = latest_version;
    next.dismissals = 1;
  } else {
    next.version = state.version;
    next.dismissals = state.dismissals >= 2 ? 2 : state.dismissals + 1;
  }
  next.dismissed_ms = now_ms;
  return next;
}

} // namespace pbr
