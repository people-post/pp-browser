#pragma once

#include <cstdint>

namespace pbr {

/** The Chats page's "save your recovery phrase" card comes back once, this long after the first "Later". */
constexpr int64_t kRecoveryReminderRepeatMs = 7LL * 24 * 60 * 60 * 1000;

struct RecoveryReminderInput {
  /** The first-run sheets are done (never over them). */
  bool onboarding_completed = false;
  /** This device can show the phrase (a linked, non-admin device has none). */
  bool can_view_phrase = false;
  /** The phrase has been opened once (recovery_phrase_seen). */
  bool phrase_seen = false;
  /** How often "Later" was tapped, and when last. */
  int dismissals = 0;
  int64_t dismissed_ms = 0;
  int64_t now_ms = 0;
};

/** Shown until the phrase is opened; "Later" hides it, it returns once after 7 days, then never. */
inline bool ShouldShowRecoveryReminder(const RecoveryReminderInput& in) {
  if (!in.onboarding_completed || !in.can_view_phrase || in.phrase_seen) {
    return false;
  }
  if (in.dismissals <= 0) {
    return true;
  }
  return in.dismissals == 1 && in.now_ms - in.dismissed_ms >= kRecoveryReminderRepeatMs;
}

} // namespace pbr
