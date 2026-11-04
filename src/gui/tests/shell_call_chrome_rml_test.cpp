#include "gui/shell/ShellCallChromeRml.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

bool Has(const std::string& rml, const std::string& part) {
  return rml.find(part) != std::string::npos;
}

TEST(ShellCallChromeRmlTest, NothingIsMountedWhileNoCallIsUp) {
  const ShellState idle;
  EXPECT_TRUE(CallRingRml(idle).empty());
  EXPECT_TRUE(CallInProgressRml(idle).empty());
}

TEST(ShellCallChromeRmlTest, ARingingCallOffersItsAnswers) {
  ShellState state;
  state.call_ring.active = true;
  const std::string rml = CallRingRml(state);
  EXPECT_TRUE(Has(rml, "call_accept"));
  EXPECT_TRUE(Has(rml, "call_decline()"));
  EXPECT_TRUE(Has(rml, "data-model=\"window\""));
}

TEST(ShellCallChromeRmlTest, EachModeHasItsOwnChrome) {
  ShellState state;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Expanded;
  EXPECT_TRUE(Has(CallInProgressRml(state), "shell-call-chrome-root"));
  state.call_in_progress.mode = CallChromeMode::Minimized;
  const std::string minimized = CallInProgressRml(state);
  state.call_in_progress.mode = CallChromeMode::Immersive;
  const std::string immersive = CallInProgressRml(state);
  EXPECT_NE(minimized, immersive);
  EXPECT_TRUE(Has(minimized, "call_leave()"));
  EXPECT_TRUE(Has(immersive, "call_leave()"));
}

TEST(ShellCallChromeRmlTest, TheFlipButtonSitsNextToTheCameraBehindItsOwnGate) {
  ShellState state;
  state.call_in_progress.active = true;
  for (const CallChromeMode mode : {CallChromeMode::Expanded, CallChromeMode::Immersive}) {
    state.call_in_progress.mode = mode;
    const std::string rml = CallInProgressRml(state);
    EXPECT_TRUE(Has(rml, "data-if=\"call_in_progress_show_flip\" data-event-click=\"call_flip_camera()\""));
    EXPECT_TRUE(Has(rml, "../icons/camera-flip.svg"));
    EXPECT_LT(rml.find("call_camera()"), rml.find("call_flip_camera()"));
  }
}

TEST(ShellCallChromeRmlTest, ARingingOutgoingCallHasALabelledCancelThatEndsIt) {
  ShellState state;
  state.call_in_progress.active = true;
  for (const CallChromeMode mode : {CallChromeMode::Expanded, CallChromeMode::Immersive}) {
    state.call_in_progress.mode = mode;
    const std::string rml = CallInProgressRml(state);
    const size_t row = rml.find("data-if=\"call_in_progress_calling\"");
    ASSERT_NE(row, std::string::npos);
    EXPECT_NE(rml.find("class=\"shell-call-cancel\" type=\"button\" data-event-click=\"call_leave()\"", row),
              std::string::npos);
  }
}

} // namespace
} // namespace pbr
