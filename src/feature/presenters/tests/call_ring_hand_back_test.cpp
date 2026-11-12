#include "feature/presenters/CallRingHandBack.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

namespace pbr {
namespace {

/** Every condition for a hand-back holds. */
CallRingHandBackInput AllHold() {
  CallRingHandBackInput in;
  in.system_rings = true;
  in.answered_on_system = false;
  in.pending_push_call = false;
  in.ui = {.app_foreground = true,
           .screen_known = true,
           .screen_interactive = true,
           .keyguard_locked = false,
           .pin_locked = false};
  return in;
}

TEST(CallRingHandBackTest, AllConditionsForTheSettleTimeHandBack) {
  EXPECT_TRUE(CallRingHandBack(AllHold(), kCallRingHandBackSettleMs));
  EXPECT_TRUE(CallRingHandBack(AllHold(), kCallRingHandBackSettleMs + 5'000));
}

TEST(CallRingHandBackTest, LessThanTheSettleTimeDoesNot) {
  EXPECT_FALSE(CallRingHandBack(AllHold(), 0));
  EXPECT_FALSE(CallRingHandBack(AllHold(), kCallRingHandBackSettleMs - 1));
  EXPECT_TRUE(CallRingHandBackConditions(AllHold())) << "the conditions hold; only the time is short";
}

// Each input turned the wrong way once: no hand-back however long it lasts.
TEST(CallRingHandBackTest, EachConditionFlippedKeepsTheRingOnTheSystemScreen) {
  struct Case {
    std::string name;
    std::function<void(CallRingHandBackInput&)> flip;
  };
  const std::vector<Case> cases = {
      {"the system screen rings nothing for us", [](CallRingHandBackInput& in) { in.system_rings = false; }},
      {"answered on the notification", [](CallRingHandBackInput& in) { in.answered_on_system = true; }},
      {"a call push waits for its ring", [](CallRingHandBackInput& in) { in.pending_push_call = true; }},
      {"not in the foreground", [](CallRingHandBackInput& in) { in.ui.app_foreground = false; }},
      {"the screen is dark", [](CallRingHandBackInput& in) { in.ui.screen_interactive = false; }},
      {"the phone is locked", [](CallRingHandBackInput& in) { in.ui.keyguard_locked = true; }},
      {"the screen query failed", [](CallRingHandBackInput& in) { in.ui.screen_known = false; }},
      {"PP is behind its PIN", [](CallRingHandBackInput& in) { in.ui.pin_locked = true; }},
  };
  for (const Case& c : cases) {
    CallRingHandBackInput in = AllHold();
    c.flip(in);
    EXPECT_FALSE(CallRingHandBackConditions(in)) << c.name;
    EXPECT_FALSE(CallRingHandBack(in, kCallRingHandBackSettleMs * 100)) << c.name;
  }
}

TEST(CallRingHandBackTest, TheUiIsSeenOnlyInFrontOnALitUnlockedScreenWithoutThePin) {
  const CallRingUi seen = AllHold().ui;
  EXPECT_TRUE(CallRingUiVisible(seen));
  CallRingUi failed = seen;
  failed.screen_known = false;
  failed.screen_interactive = true; // whatever else it says, a failed query is not seen
  failed.keyguard_locked = false;
  EXPECT_FALSE(CallRingUiVisible(failed));
  CallRingUi pin = seen;
  pin.pin_locked = true;
  EXPECT_FALSE(CallRingUiVisible(pin));
  EXPECT_FALSE(CallRingUiVisible(CallRingUi{})) << "the defaults are not seen";
}

} // namespace
} // namespace pbr
