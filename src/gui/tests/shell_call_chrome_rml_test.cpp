#include "gui/shell/ShellCallChromeRml.h"
#include "gui/CallChromeSync.h"

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

TEST(ShellCallChromeRmlTest, MetersAreOnlyInTheExpandedBar) {
  // CallChromeSync treats a level change as invisible outside Expanded (B146): keep the markup honest.
  ShellState state;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Expanded;
  const std::string expanded = CallInProgressRml(state);
  state.call_in_progress.mode = CallChromeMode::Minimized;
  const std::string minimized = CallInProgressRml(state);
  state.call_in_progress.mode = CallChromeMode::Immersive;
  const std::string immersive = CallInProgressRml(state);
  for (const char* name : kCallLevelVariables) {
    EXPECT_TRUE(Has(expanded, name)) << name;
    EXPECT_FALSE(Has(minimized, name)) << name;
    EXPECT_FALSE(Has(immersive, name)) << name;
  }
  EXPECT_TRUE(Has(expanded, "data-if=\"!call_in_progress_show_roster\"")); // and hidden under a roster
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

// V059: Immersive's grid cards spotlight their person on a tap; with a spotlight, its card fills the call area
// (drawn fill or fit, a double tap toggles), the others line up in a strip, and a grid button goes back.
TEST(ShellCallChromeRmlTest, ImmersiveSpotlightsATappedPersonAndCanGoBackToTheGrid) {
  ShellState state;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Immersive;
  const std::string grid = CallInProgressRml(state);
  EXPECT_TRUE(Has(grid, "data-for=\"p : call_in_progress_roster\""));
  EXPECT_TRUE(Has(grid, "data-event-click=\"call_spotlight(p.stream_id)\""));
  EXPECT_FALSE(Has(grid, "call_spotlight_grid()")) << "no grid button on the grid";
  EXPECT_FALSE(Has(grid, "call_in_progress_spotlight_rows"));

  state.call_in_progress.spotlight_stream = "22";
  state.call_in_progress.spotlight_fill = true;
  const std::string spotlight = CallInProgressRml(state);
  EXPECT_TRUE(Has(spotlight, "data-for=\"p : call_in_progress_spotlight_rows\""));
  EXPECT_TRUE(Has(spotlight, "data-for=\"p : call_in_progress_strip_rows\""));
  EXPECT_TRUE(Has(spotlight, "data-event-dblclick=\"call_spotlight_fit()\""));
  EXPECT_TRUE(Has(spotlight, "call_spotlight_grid()"));
  EXPECT_TRUE(Has(spotlight, "fit=\"fill\""));
  EXPECT_FALSE(Has(spotlight, "data-for=\"p : call_in_progress_roster\"")) << "the grid is gone";

  state.call_in_progress.spotlight_fill = false;
  const std::string fit = CallInProgressRml(state);
  const size_t spot = fit.find("call_in_progress_spotlight_rows");
  const size_t strip = fit.find("call_in_progress_strip_rows");
  ASSERT_NE(spot, std::string::npos);
  ASSERT_NE(strip, std::string::npos);
  EXPECT_NE(fit.substr(spot, strip - spot).find("fit=\"fit\""), std::string::npos) << "the spotlight letterboxed";
}

} // namespace
} // namespace pbr
