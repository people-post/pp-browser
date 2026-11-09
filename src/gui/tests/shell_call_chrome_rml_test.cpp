#include "gui/shell/ShellCallChromeRml.h"
#include "gui/CallChromeSync.h"
#include "lib/ui/CallChipDock.h"

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

// #165: the connection story always shows all three steps; the reached ones are highlighted (done = ✓, current =
// moving dot), the ones not reached yet are plain dots. Same for the 1:1 page and the group card.
std::string StepMarkup(const char* modifier, const bool check, const int n) {
  return std::string("<div class=\"call-phone-step") + modifier + "\"><div class=\"call-phone-step-dot\">" +
         (check ? "<svg src=\"../icons/v2/check.svg\" width=\"12\" height=\"12\"></svg>" : "") +
         "</div><p class=\"call-phone-step-text\">{{call_in_progress_story_step" + std::to_string(n) + "}}</p></div>";
}

TEST(ShellCallChromeRmlTest, PhoneStoryShowsAllStepsAndHighlightsTheReachedOnes) {
  const std::string done = " call-phone-step--done";
  const std::string current = " call-phone-step--current";
  // expected[stage][step]: 'd' done, 'c' current, 'p' plain.
  const char* expected[] = {"cpp", "dcp", "ddc", "ddd"};
  for (const bool roster : {false, true}) {  // false = 1:1 page, true = group card
    for (int stage = 0; stage < 4; ++stage) {
      ShellState state = PhoneGroupCall();
      state.call_in_progress.show_roster = roster;
      state.call_in_progress.story_active = true;
      state.call_in_progress.story_stage = stage;
      const std::string rml = CallInProgressRml(state);
      EXPECT_EQ(Count(rml, "<div class=\"call-phone-step-dot\">"), 3u) << "roster=" << roster << " stage=" << stage;
      size_t last = 0;
      for (int i = 0; i < 3; ++i) {
        const char kind = expected[stage][i];
        const std::string want = StepMarkup(kind == 'd' ? done.c_str() : kind == 'c' ? current.c_str() : "",
                                            kind == 'd', i + 1);
        const size_t at = rml.find(want);
        EXPECT_NE(at, std::string::npos) << "roster=" << roster << " stage=" << stage << " step=" << i + 1;
        EXPECT_GE(at, last);  // in order
        last = at;
      }
      EXPECT_EQ(Count(rml, "call-phone-step--current"), stage < 3 ? 1u : 0u);
    }
  }

  // #172: a 1:1 call's five steps: calling, answered, matching, encrypted channel, connected (5 = all done).
  const char* five[] = {"cpppp", "dcppp", "ddcpp", "dddcp", "ddddd"};
  const int stages[] = {0, 1, 2, 3, 5};
  for (int k = 0; k < 5; ++k) {
    ShellState state = PhoneGroupCall();
    state.call_in_progress.show_roster = false;
    state.call_in_progress.story_active = true;
    state.call_in_progress.story_step_count = 5;
    state.call_in_progress.story_stage = stages[k];
    const std::string rml = CallInProgressRml(state);
    EXPECT_EQ(Count(rml, "<div class=\"call-phone-step-dot\">"), 5u) << "stage=" << stages[k];
    size_t last = 0;
    for (int i = 0; i < 5; ++i) {
      const char kind = five[k][i];
      const std::string want = StepMarkup(kind == 'd' ? done.c_str() : kind == 'c' ? current.c_str() : "",
                                          kind == 'd', i + 1);
      const size_t at = rml.find(want);
      EXPECT_NE(at, std::string::npos) << "five steps stage=" << stages[k] << " step=" << i + 1;
      EXPECT_GE(at, last);
      last = at;
    }
  }

  // "How this call got through" still lists every line so far.
  ShellState state = PhoneGroupCall();
  state.call_in_progress.show_roster = false;
  state.call_in_progress.story_active = true;
  state.call_in_progress.story_sheet_open = true;
  state.call_in_progress.story_lines = {{.time = "10:02", .text = "You called"},
                                        {.time = "3.0 s", .text = "They answered"},
                                        {.time = "4.8 s", .text = "Connected directly", .secure = true}};
  const std::string sheet = CallInProgressRml(state);
  EXPECT_EQ(Count(sheet, "<div class=\"call-phone-event-dot\">"), 3u);
  EXPECT_TRUE(Has(sheet, "You called"));
  EXPECT_TRUE(Has(sheet, "They answered"));
  EXPECT_TRUE(Has(sheet, "Connected directly"));
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

// #83: the flip button is lit while the front camera is in use and plain on the rear one (a bound class, no animation).
TEST(ShellCallChromeRmlTest, PhoneVideoFlipButtonIsLitOnTheFrontCamera) {
  ShellState state = PhoneGroupCall();
  state.call_in_progress.show_roster = false;
  state.call_in_progress.stage_visible = true;
  state.call_in_progress.show_flip = true;
  const std::string rml = CallInProgressRml(state);
  EXPECT_TRUE(Has(rml, "data-event-click=\"call_flip_camera()\" "
                       "data-class-call-phone-control--on=\"call_in_progress_camera_front\""));
}

// #189: the minimised chip is the full bar (title, timer, quality, mute, hang-up) ...
TEST(ShellCallChromeRmlTest, TheMinimisedFullBarHasTitleTimerQualityAndButtons) {
  ShellState state;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Minimized;
  const std::string rml = CallInProgressRml(state);
  EXPECT_TRUE(Has(rml, "call_in_progress_title"));
  EXPECT_TRUE(Has(rml, "call_in_progress_subtitle"));
  EXPECT_TRUE(Has(rml, "shell-call-quality"));
  EXPECT_TRUE(Has(rml, "call_mute()"));
  EXPECT_TRUE(Has(rml, "call_leave()"));
  EXPECT_FALSE(Has(rml, "--handle"));
  // ... also when it sits on a side, until it is tucked.
  state.call_in_progress.minimized_side = CallChipDock::kSideRight;
  state.call_in_progress.minimized_y_permille = 500;
  EXPECT_TRUE(Has(CallInProgressRml(state), "call_leave()"));
}

// ... and, thrown to a side edge, the handle: only the phone, 12dp of it beyond that edge.
TEST(ShellCallChromeRmlTest, TheMinimisedHandleIsOnlyTheIconHangingOnItsEdge) {
  ShellState state;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Minimized;
  state.call_in_progress.minimized_side = CallChipDock::kSideRight;
  state.call_in_progress.minimized_y_permille = 500;
  state.call_in_progress.minimized_tucked = true;
  const std::string right = CallInProgressRml(state);
  EXPECT_TRUE(Has(right, "shell-call-minimized-chip--handle"));
  EXPECT_TRUE(Has(right, "shell-call-minimized-icon"));
  EXPECT_TRUE(Has(right, "../icons/v2/phone.svg"));
  EXPECT_TRUE(Has(right, "right:-12dp"));
  EXPECT_FALSE(Has(right, "call_in_progress_title"));
  EXPECT_FALSE(Has(right, "call_in_progress_subtitle"));
  EXPECT_FALSE(Has(right, "<button"));
  state.call_in_progress.minimized_side = CallChipDock::kSideLeft;
  EXPECT_TRUE(Has(CallInProgressRml(state), "left:-12dp"));
  // The default corner has no edge to hang on: always the bar.
  state.call_in_progress.minimized_side = CallChipDock::kSideDefault;
  EXPECT_FALSE(Has(CallInProgressRml(state), "--handle"));
}

// #166: the minimised chip. Never dragged this call: the default top-right corner class, no inline place — as before.
// Dropped on a side: anchored on that side by an inline style, its top from CallChipDock::Place, no corner class.
TEST(ShellCallChromeRmlTest, TheMinimisedChipKeepsItsCornerUntilDraggedThenSitsWhereItWasDropped) {
  for (const LayoutMode layout : {LayoutMode::Compact, LayoutMode::Expanded}) {
    ShellState state;
    state.layout_mode = layout;
    state.call_in_progress.active = true;
    state.call_in_progress.mode = CallChromeMode::Minimized;
    const std::string corner = CallInProgressRml(state);
    EXPECT_TRUE(Has(corner, "class=\"shell-call-minimized-chip row shell-call-minimized-chip--tr\">"));
    EXPECT_FALSE(Has(corner, "style=")) << "no inline place before a drag";

    // The layer and the chip as last laid out (ShellHost::MeasureCallChip): the band is [12, 780 - 72 - 56 = 652].
    state.call_chip_layout.area_height_dp = 780.f;
    state.call_chip_layout.chip_height_dp = 56.f;
    state.call_in_progress.minimized_side = CallChipDock::kSideRight;
    state.call_in_progress.minimized_y_permille = 500;
    const std::string right = CallInProgressRml(state);
    EXPECT_TRUE(Has(right, "style=\"right:12dp;left:auto;top:332.0dp;bottom:auto;\"")) << right;
    EXPECT_FALSE(Has(right, "shell-call-minimized-chip--"));

    state.call_in_progress.minimized_side = CallChipDock::kSideLeft;
    state.call_in_progress.minimized_y_permille = 1000;
    const std::string left = CallInProgressRml(state);
    EXPECT_TRUE(Has(left, "style=\"left:12dp;right:auto;top:652.0dp;bottom:auto;\"")) << left;
    EXPECT_FALSE(Has(left, "shell-call-minimized-chip--"));

    // The chip grew since (the quality words): re-clamped, its bottom still 72dp above the layer's.
    state.call_chip_layout.chip_height_dp = 80.f;
    EXPECT_TRUE(Has(CallInProgressRml(state), "top:628.0dp;"));
  }
}

// #166 (iPhone, 2026-11-08): a dropped chip showed at the top of its side and then moved down to where it was dropped
// (a rebuild placed it with no height measured: the band's top). The markup now reuses where the gesture put it, and
// never falls back to the band's top.
TEST(ShellCallChromeRmlTest, ARebuiltChipKeepsTheTopTheGesturePutItAtAndNeverFallsBackToTheBandsTop) {
  ShellState state;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Minimized;
  state.call_in_progress.minimized_side = CallChipDock::kSideRight;
  state.call_in_progress.minimized_y_permille = 545;

  // Nothing measured at all: no top (never "top:12.0dp", the band's top), the side anchor still there.
  const std::string unknown = CallInProgressRml(state);
  EXPECT_TRUE(Has(unknown, "style=\"right:12dp;left:auto;bottom:auto;\"")) << unknown;
  EXPECT_FALSE(Has(unknown, "top:"));

  // The gesture's top for this place, with no heights in the state: exactly that top.
  state.call_chip_layout.place = {CallChipDock::kSideRight, 545};
  state.call_chip_layout.top_dp = 372.2f;
  EXPECT_TRUE(Has(CallInProgressRml(state), "style=\"right:12dp;left:auto;top:372.2dp;bottom:auto;\""));

  // With the heights it was worked out with: the same top (inside the band).
  state.call_chip_layout.area_height_dp = 800.f;
  state.call_chip_layout.chip_height_dp = 56.f;
  EXPECT_TRUE(Has(CallInProgressRml(state), "top:372.2dp;"));
  // The window got shorter since: kept in the band.
  state.call_chip_layout.area_height_dp = 400.f;
  EXPECT_TRUE(Has(CallInProgressRml(state), "top:272.0dp;")); // 400 - 72 - 56

  // A top for another place (an older drop): the share of the band instead, with the heights known.
  state.call_chip_layout.area_height_dp = 800.f;
  state.call_in_progress.minimized_y_permille = 1000;
  EXPECT_TRUE(Has(CallInProgressRml(state), "top:672.0dp;")); // 800 - 72 - 56
}

// The group video page has the flip button too, lit the same way, next to the camera; none while the camera is off.
TEST(ShellCallChromeRmlTest, PhoneGroupVideoHasTheFlipButtonNextToTheCamera) {
  ShellState state = PhoneGroupCall();
  state.call_in_progress.show_camera = true;
  state.call_in_progress.show_flip = true;
  const std::string rml = CallInProgressRml(state);
  const size_t flip = rml.find("data-event-click=\"call_flip_camera()\" "
                               "data-class-call-phone-control--on=\"call_in_progress_camera_front\"");
  ASSERT_NE(flip, std::string::npos);
  EXPECT_LT(rml.find("data-event-click=\"call_camera()\""), flip);
  EXPECT_LT(flip, rml.find("data-event-click=\"call_leave()\""));

  state.call_in_progress.show_flip = false;
  EXPECT_FALSE(Has(CallInProgressRml(state), "call_flip_camera()"));
}

} // namespace
} // namespace pbr
