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

} // namespace
} // namespace pbr
