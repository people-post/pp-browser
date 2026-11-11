#include "feature/presenters/CallRingHandBack.h"

namespace pbr {

bool CallRingUiVisible(const CallRingUi& ui) {
  return ui.app_foreground && ui.screen_known && ui.screen_interactive && !ui.keyguard_locked && !ui.pin_locked;
}

bool CallRingHandBackConditions(const CallRingHandBackInput& in) {
  return in.system_rings && !in.answered_on_system && !in.pending_push_call && CallRingUiVisible(in.ui);
}

bool CallRingHandBack(const CallRingHandBackInput& in, const int64_t held_ms) {
  return CallRingHandBackConditions(in) && held_ms >= kCallRingHandBackSettleMs;
}

} // namespace pbr
