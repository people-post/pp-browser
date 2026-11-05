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
    // The small round End hides while the wide Cancel shows, and comes back once the call connects.
    EXPECT_NE(rml.find("data-class-shell-call-leave--ringing=\"call_in_progress_calling\""), std::string::npos);
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

/** A phone group call, connected, of us ("11") and one other ("22"). */
ShellState PhoneGroupCall() {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Immersive;
  state.call_in_progress.show_roster = true;
  state.call_in_progress.show_invite = true;
  state.call_in_progress.roster.resize(2);
  state.call_in_progress.roster[0].is_local = true;
  state.call_in_progress.roster[0].stream_id = "11";
  state.call_in_progress.roster[1].stream_id = "22";
  return state;
}

size_t Count(const std::string& rml, const std::string& part) {
  size_t n = 0;
  for (size_t at = rml.find(part); at != std::string::npos; at = rml.find(part, at + 1)) {
    ++n;
  }
  return n;
}

// V059 on the phone: without a spotlight the 2x2 grid, a tap on a tile spotlights that person, and whoever speaks
// has a ring (a bound class per row, not markup: B146).
TEST(ShellCallChromeRmlTest, PhoneGroupGridTapsToSpotlightAndRingsWhoSpeaks) {
  const ShellState state = PhoneGroupCall();
  const std::string rml = CallInProgressRml(state);
  ASSERT_TRUE(Has(rml, "id=\"call-phone-page\""));
  EXPECT_TRUE(Has(rml,
                  "data-for=\"p : call_in_progress_roster\" class=\"call-phone-tile\" "
                  "data-class-call-phone-tile--on-hold=\"p.on_hold\" "
                  "data-class-call-phone-tile--speaking=\"p.speaking\" "
                  "data-event-click=\"call_spotlight(p.stream_id)\""));
  EXPECT_FALSE(Has(rml, "call_in_progress_spotlight_rows"));
  EXPECT_FALSE(Has(rml, "call_in_progress_strip_rows"));
  EXPECT_FALSE(Has(rml, "call_spotlight_grid()")) << "no grid button on the grid";
}

// With a spotlight the person fills the area (a double tap: fill / fit), the others line up in a strip that taps to
// spotlight them, the grid button sits in the top row, and the rings are on both.
TEST(ShellCallChromeRmlTest, PhoneGroupSpotlightFillsTheAreaWithAStripAndAGridButton) {
  ShellState state = PhoneGroupCall();
  state.call_in_progress.spotlight_stream = "22";
  state.call_in_progress.spotlight_fill = true;
  const std::string rml = CallInProgressRml(state);
  EXPECT_FALSE(Has(rml, "data-for=\"p : call_in_progress_roster\"")) << "the grid is gone";
  EXPECT_TRUE(Has(rml, "data-for=\"p : call_in_progress_spotlight_rows\" class=\"call-phone-tile "
                       "call-phone-tile--spotlit\""));
  EXPECT_TRUE(Has(rml, "data-event-dblclick=\"call_spotlight_fit()\""));
  EXPECT_TRUE(Has(rml, "data-for=\"p : call_in_progress_strip_rows\" class=\"call-phone-tile "
                       "call-phone-tile--strip\""));
  EXPECT_EQ(Count(rml, "data-event-click=\"call_spotlight(p.stream_id)\""), 1u) << "the strip's tap";
  EXPECT_EQ(Count(rml, "data-class-call-phone-tile--speaking=\"p.speaking\""), 2u) << "spotlight and strip";
  const size_t grid = rml.find("data-event-click=\"call_spotlight_grid()\"");
  ASSERT_NE(grid, std::string::npos);
  EXPECT_LT(grid, rml.find("call_invite()")) << "the grid button before add-people";
  EXPECT_LT(rml.find("call-phone-strip"), rml.find("call-phone-controls")) << "the strip above the buttons";
  const size_t spot = rml.find("call_in_progress_spotlight_rows");
  EXPECT_NE(rml.substr(spot, rml.find("call-phone-strip") - spot).find("fit=\"fill\""), std::string::npos);

  state.call_in_progress.spotlight_fill = false;
  const std::string fit = CallInProgressRml(state);
  const size_t fit_spot = fit.find("call_in_progress_spotlight_rows");
  EXPECT_NE(fit.substr(fit_spot, fit.find("call-phone-strip") - fit_spot).find("fit=\"fit\""), std::string::npos)
      << "the spotlight letterboxed";
}

// Before connect the group page keeps its dimmed grid under the step card: no spotlight, no taps, no rings.
TEST(ShellCallChromeRmlTest, PhoneGroupBeforeConnectKeepsTheDimmedGrid) {
  ShellState state = PhoneGroupCall();
  state.call_in_progress.story_active = true;
  state.call_in_progress.spotlight_stream = "22";
  const std::string rml = CallInProgressRml(state);
  EXPECT_TRUE(Has(rml, "call-phone-grid call-phone-grid--dim"));
  EXPECT_TRUE(Has(rml, "data-for=\"p : call_in_progress_roster\""));
  EXPECT_FALSE(Has(rml, "call_in_progress_spotlight_rows"));
  EXPECT_FALSE(Has(rml, "call_spotlight"));
  EXPECT_FALSE(Has(rml, "p.speaking"));
}

// V060: a person on hold has "On hold" on their tile (dimmed), and a paused camera says so — per row, bound.
TEST(ShellCallChromeRmlTest, PhoneGroupTilesShowWhoIsOnHoldAndWhoseCameraIsPaused) {
  const std::string rml = CallInProgressRml(PhoneGroupCall());
  EXPECT_TRUE(Has(rml, "<div class=\"call-hold-pill call-hold-pill--tile\" data-if=\"p.on_hold\">"));
  EXPECT_TRUE(Has(rml, "data-class-call-phone-tile--on-hold=\"p.on_hold\""));
  EXPECT_TRUE(Has(rml, "data-if=\"p.video_paused && !p.on_hold\""));
}

// V060: the 1:1 voice page shows "On hold" under the name while the call is on hold on either side.
TEST(ShellCallChromeRmlTest, PhoneVoiceCallShowsOnHoldUnderTheName) {
  ShellState state = PhoneGroupCall();
  state.call_in_progress.show_roster = false;
  const std::string rml = CallInProgressRml(state);
  EXPECT_TRUE(Has(rml, "<div class=\"call-hold-pill call-hold-pill--hero\" data-if=\"call_in_progress_on_hold\">"));
}

// V060: desktop cards and the roster chips show hold too.
TEST(ShellCallChromeRmlTest, DesktopCardsAndChipsShowWhoIsOnHold) {
  ShellState state = PhoneGroupCall();
  state.layout_mode = LayoutMode::Expanded;
  const std::string immersive = CallInProgressRml(state);
  EXPECT_TRUE(Has(immersive, "data-class-shell-call-peer-card--on-hold=\"p.on_hold\""));
  EXPECT_TRUE(Has(immersive, "<div class=\"call-hold-pill\" data-if=\"p.on_hold\">"));
  state.call_in_progress.mode = CallChromeMode::Expanded;
  EXPECT_TRUE(Has(CallInProgressRml(state), "data-if=\"p.on_hold\" src=\"../icons/pause.svg\""));
}

// 1:1 video: their camera big, ours small; a tap on the small one swaps them (spotlights the one it shows).
TEST(ShellCallChromeRmlTest, PhoneVideoCallSwapsOnATapOnTheSmallPicture) {
  ShellState state = PhoneGroupCall();
  state.call_in_progress.show_roster = false;
  state.call_in_progress.stage_visible = true;
  state.call_in_progress.spotlight_stream = "22";
  const std::string normal = CallInProgressRml(state);
  EXPECT_TRUE(Has(normal, "class=\"call-phone-pip\" id=\"call-local-tile\" tile=\"local\" "
                          "data-if=\"call_in_progress_local_preview\" data-event-click=\"call_spotlight('11')\""));
  EXPECT_TRUE(Has(normal, "class=\"call-phone-remote\" id=\"call-remote-tile\" tile=\"remote\">"));
  EXPECT_TRUE(Has(normal, "<div class=\"call-hold-pill call-hold-pill--who\" data-if=\"call_in_progress_on_hold\">"))
      << "V060: their picture may still run while they are on hold";

  state.call_in_progress.spotlight_stream = "11";
  const std::string swapped = CallInProgressRml(state);
  EXPECT_TRUE(Has(swapped, "<call-video-tile class=\"call-phone-remote\" tile=\"local\" "
                           "data-if=\"call_in_progress_local_preview\">"));
  EXPECT_TRUE(Has(swapped, "tile=\"remote\" data-if=\"!call_in_progress_local_preview\"")) << "camera off: theirs";
  EXPECT_TRUE(Has(swapped, "class=\"call-phone-pip\" tile=\"remote\" "
                           "data-if=\"call_in_progress_local_preview\" data-event-click=\"call_spotlight('22')\""));
  EXPECT_FALSE(Has(swapped, "call_spotlight_grid()"));
}

} // namespace
} // namespace pbr
