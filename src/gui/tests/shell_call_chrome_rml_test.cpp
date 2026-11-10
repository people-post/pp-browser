#include "gui/shell/ShellCallChromeRml.h"
#include "gui/CallChromeSync.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ui/CallChipDock.h"

#include "feature/presenters/CallStepScroll.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

bool Has(const std::string& rml, const std::string& part) {
  return rml.find(part) != std::string::npos;
}

size_t Count(const std::string& rml, const std::string& part);
ShellState PhoneGroupCall();
std::string ReadAll(const std::string& path);

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

// Incoming call, connecting…: the same phone ring page before and after the invite arrives. Everything that differs
// is bound (the dim class, the avatar variants), so the markup is identical in both states and no remount is needed.
TEST(ShellCallChromeRmlTest, TheAwaitingAndTheRealRingShareOneMarkup) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_ring.active = true;
  state.call_ring.awaiting_invite = true;
  const std::string waiting = CallRingRml(state);
  EXPECT_TRUE(Has(waiting, "call-phone--ring"));
  EXPECT_TRUE(Has(waiting, "{{call_ring_phone_title}}"));
  state.call_ring.awaiting_invite = false;
  state.call_ring.has_avatar = true;
  state.call_ring.avatar_src = "icons/bob.png";
  state.call_ring.avatar_letter = "B";
  // The three lines, our own avatar and the name turning over are all bound values too.
  state.call_ring.phone_title = "Bob";
  state.call_ring.step1 = "Voice call received"; // the first row turns "Call received" into voice / video: bound
  state.call_ring.step2_current = true;
  state.call_ring.step3_done = true;
  state.call_ring.step3 = "Bob";
  state.call_ring.self_has_avatar = true;
  state.call_ring.self_avatar_src = "icons/me.png";
  state.call_ring.self_avatar_letter = "M";
  state.call_ring.self_avatar_tone = 5;
  EXPECT_EQ(waiting, CallRingRml(state));
}

TEST(ShellCallChromeRmlTest, TheRingPageDimsItsAnswersByABoundClassAndKeepsTheirHandlers) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_ring.active = true;
  const std::string rml = CallRingRml(state);
  EXPECT_TRUE(Has(rml, "data-class-call-phone-answer--waiting=\"call_ring_awaiting_invite\""));
  EXPECT_EQ(rml.find("call-phone-answer-action\" data-class-call-phone-answer--waiting"),
            rml.find("call-phone-answer-action"));
  EXPECT_TRUE(Has(rml, "call_accept()"));
  EXPECT_TRUE(Has(rml, "call_accept_voice()"));
  EXPECT_TRUE(Has(rml, "call_decline()"));
}

// The 1:1 ring page: us on the left, them on the right at the pair's size; their avatar is a silhouette while awaiting
// (or for a stranger), then photo or letter, all by bound switches; a group's big icon is its own block.
TEST(ShellCallChromeRmlTest, TheRingAvatarIsASilhouetteWhileAwaitingThenPhotoOrLetterByBoundSwitches) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_ring.active = true;
  const std::string rml = CallRingRml(state);
  EXPECT_TRUE(Has(rml,
                  "call-phone-avatar--unknown avatar-tone-0 call-phone-avatar--pair\" "
                  "data-if=\"call_ring_awaiting_invite || call_ring_caller_unknown\""));
  EXPECT_TRUE(Has(rml, "icons/v2/unknown-caller.svg"));
  EXPECT_TRUE(Has(rml, "data-if=\"!call_ring_awaiting_invite && !call_ring_caller_unknown && call_ring_has_avatar\""));
  EXPECT_TRUE(Has(rml, "data-attr-src=\"call_ring_avatar_src\""));
  EXPECT_TRUE(Has(rml, "data-if=\"!call_ring_awaiting_invite && !call_ring_caller_unknown && !call_ring_has_avatar\""));
  EXPECT_TRUE(Has(rml, "data-class-avatar-tone-7=\"call_ring_avatar_tone == 7\""));
  EXPECT_TRUE(Has(rml, "{{call_ring_avatar_letter}}"));
  EXPECT_TRUE(Has(rml, "data-if=\"!call_ring_group\"")) << "the 1:1 block";
  EXPECT_TRUE(Has(rml, "call-phone-hero call-phone-hero--ring\" data-if=\"call_ring_group\"")) << "the group block";
  EXPECT_TRUE(Has(rml, "call-phone-avatar--group call-phone-avatar--big"));
}

// A caller nobody knows by name (no contact, no photo) is "Stranger" with the silhouette on the ring page too, by the
// same bound switch as the awaiting page; a photo or a name keeps the usual avatar.
TEST(ShellCallChromeRmlTest, TheRingPageShowsTheSilhouetteForAnUnknownCallerByTheBoundFlag) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_ring.active = true;
  state.call_ring.caller_unknown = true;
  const std::string rml = CallRingRml(state);
  EXPECT_TRUE(Has(rml, "data-if=\"call_ring_awaiting_invite || call_ring_caller_unknown\""));
  EXPECT_TRUE(Has(rml, "data-if=\"!call_ring_awaiting_invite && !call_ring_caller_unknown && !call_ring_has_avatar\""))
      << "the letter variant steps aside for it";
  state.call_ring.caller_unknown = false;
  EXPECT_EQ(rml, CallRingRml(state)) << "bound: the markup is the same, no remount";
}

// Us on the left (photo or letter, bound: before the unlock there is no profile), them on the right, the spark running
// from them to us; their name is the bold one, "You" the small one.
TEST(ShellCallChromeRmlTest, TheRingPairHasUsFirstThemSecondAndTheSparkComesTowardsUs) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_ring.active = true;
  const std::string rml = CallRingRml(state);
  const size_t pair = rml.find("class=\"call-phone-pair\"");
  const size_t me = rml.find("data-attr-src=\"call_ring_self_avatar_src\"");
  const size_t path = rml.find("call-phone-pair-path call-phone-pair-path--inbound");
  const size_t them = rml.find("unknown-caller.svg");
  const size_t names = rml.find("class=\"call-phone-pair-names\"");
  ASSERT_NE(pair, std::string::npos);
  ASSERT_NE(me, std::string::npos);
  ASSERT_NE(path, std::string::npos);
  ASSERT_NE(them, std::string::npos);
  ASSERT_NE(names, std::string::npos);
  EXPECT_LT(pair, me);
  EXPECT_LT(me, path);
  EXPECT_LT(path, them);
  EXPECT_LT(them, names);
  EXPECT_TRUE(Has(rml, "{{call_ring_self_avatar_letter}}"));
  EXPECT_TRUE(Has(rml, "data-class-avatar-tone-3=\"call_ring_self_avatar_tone == 3\""));
  const std::string row = rml.substr(names, rml.find("</div>", names) - names);
  EXPECT_LT(row.find(Tr("call.label.you")), row.find("call-phone-pair-name call-phone-pair-name--peer"));
  EXPECT_TRUE(Has(row, "call-phone-pair-name call-phone-pair-name--peer\">{{call_ring_phone_title}}"));
  EXPECT_EQ(Count(row, "call-phone-pair-name--peer"), 1u) << "only the other party's name is the big one";
}

// Three fixed rows, every difference bound: the first is always done; the second turns from current to done; the
// third from plain to done (check by data-if, text by value).
TEST(ShellCallChromeRmlTest, TheRingPageHasThreeBoundStepRowsUnderThePair) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_ring.active = true;
  const std::string rml = CallRingRml(state);
  EXPECT_EQ(Count(rml, "{{call_ring_step1}}"), 2u) << "the 1:1 block and the group block";
  EXPECT_TRUE(Has(rml, "<div class=\"call-phone-steps\" data-if=\"call_ring_show_steps\">"))
      << "a short page can give the lines' room to the price / conflict text";
  const size_t steps = rml.find("<div class=\"call-phone-steps\" data-if=\"call_ring_show_steps\">");
  ASSERT_NE(steps, std::string::npos);
  const std::string one_to_one = rml.substr(steps, rml.find("{{call_ring_step3}}", steps) - steps);
  EXPECT_EQ(Count(one_to_one, "<div class=\"call-phone-step-dot\">"), 3u);
  EXPECT_TRUE(Has(one_to_one, "call-phone-step call-phone-step--done\"><div class=\"call-phone-step-dot\"><svg"));
  EXPECT_TRUE(Has(one_to_one, "data-class-call-phone-step--current=\"call_ring_step2_current\""));
  EXPECT_TRUE(Has(one_to_one, "data-class-call-phone-step--done=\"!call_ring_step2_current\""));
  EXPECT_TRUE(Has(one_to_one, "data-if=\"!call_ring_step2_current\""));
  EXPECT_TRUE(Has(rml, "data-class-call-phone-step--done=\"call_ring_step3_done\""));
  EXPECT_TRUE(Has(rml, "data-if=\"call_ring_step3_done\""));
  EXPECT_LT(rml.find("{{call_ring_step1}}"), rml.find("{{call_ring_step2}}"));
  EXPECT_LT(rml.find("{{call_ring_step2}}"), rml.find("{{call_ring_step3}}"));
}

// The ring page and the answering page take the pair, the names and the steps from the same classes, and the ring page
// pads for the 44dp bar it does not have: the numbers that put them level are asserted against the rcss.
std::string ReadAll(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

/** The dp value of `property` in the rule `selector { ... }` of the call page rcss (-1: not found). */
int RcssDp(const std::string& rcss, const std::string& selector, const std::string& property) {
  const size_t rule = rcss.find("\n" + selector + " {");
  if (rule == std::string::npos) {
    return -1;
  }
  const size_t end = rcss.find('}', rule);
  const size_t at = rcss.find("\n  " + property + ":", rule);
  if (at == std::string::npos || at > end) {
    return -1;
  }
  return std::atoi(rcss.c_str() + at + property.size() + 4);
}

TEST(ShellCallChromeRmlTest, TheRingAndAnsweringPagesShareTheirPairNamesAndStepsClasses) {
  ShellState ring;
  ring.layout_mode = LayoutMode::Compact;
  ring.call_ring.active = true;
  const std::string ring_rml = CallRingRml(ring);
  ShellState answer = PhoneGroupCall();
  answer.call_in_progress.show_roster = false;
  answer.call_in_progress.story_active = true;
  const std::string answer_rml = CallInProgressRml(answer);
  for (const char* shared : {"class=\"call-phone-pair\"", "call-phone-pair-path", "call-phone-avatar--pair",
                             "class=\"call-phone-pair-names\"", "call-phone-pair-name call-phone-pair-name--peer",
                             "<div class=\"call-phone-steps\"", "call-phone-step-dot", "call-phone-step-text"}) {
    EXPECT_TRUE(Has(ring_rml, shared)) << shared;
    EXPECT_TRUE(Has(answer_rml, shared)) << shared;
  }
  EXPECT_TRUE(Has(ring_rml, "call-phone-story call-phone-story--ring"));
  EXPECT_FALSE(Has(answer_rml, "call-phone-story--ring"));
}

TEST(ShellCallChromeRmlTest, TheRingPagesStoryPaddingIsTheTopBarPlusTheAnsweringPagesStoryPadding) {
  const std::string rcss = ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss");
  const int bar = RcssDp(rcss, ".call-phone-top", "height");
  const int story = RcssDp(rcss, ".call-phone-story", "padding");
  ASSERT_EQ(bar, 44);
  // `padding: 59dp 24dp 0 24dp;`: the first number is the top.
  ASSERT_EQ(story, 59);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-story--ring", "padding-top"), bar + story);
}

// The scroll arithmetic's numbers (CallStepScroll.h) are the rcss's.
TEST(ShellCallChromeRmlTest, TheStepScrollConstantsAreTheRcssNumbers) {
  using namespace call_page_dp;
  const std::string rcss = ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss");
  EXPECT_EQ(RcssDp(rcss, ".call-phone-top", "height"), kTopBar);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-story", "padding"), kStoryPaddingTop);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-pair", "height"), kPairRow);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-pair-names", "margin-top"), kNamesMarginTop);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-pair-name--peer", "line-height"), kPeerNameLine);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-steps", "margin-top"), kStepsMarginTop);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-steps-clip", "margin-top"), kStepsMarginTop);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-steps", "gap"), kStepRow - RcssDp(rcss, ".call-phone-step-dot", "height"));
  EXPECT_EQ(RcssDp(rcss, ".call-phone-step-dot", "height"), 22);
  // kControlsReserve = card padding 12 + 52 circle + 4 gap + 13 label + 8, and its 22 bottom margin.
  EXPECT_TRUE(Has(rcss, "margin: 0 12dp 22dp 12dp;\n  padding: 12dp 4dp 8dp 4dp;"));
  // The ring page's own numbers (CallRingShowsSteps): the group hero, the answer bar, the charge block, the hints.
  EXPECT_EQ(RcssDp(rcss, ".call-phone-hero--ring", "padding-top"), 123);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-avatar--big", "height"), 120);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-name", "line-height"), 34);
  EXPECT_TRUE(Has(rcss, "margin: 20dp 0 4dp 0;\n  font-size: 28dp;"));
  EXPECT_EQ(RcssDp(rcss, ".call-phone-status", "line-height"), 22);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-round", "height"), 72);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-answer", "margin-bottom"), 48);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-answer-action", "gap"), 8);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-answer-label", "line-height"), kAnswerLabelLine);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-pill-btn", "height"), 50);
  EXPECT_TRUE(Has(rcss, "gap: 8dp;\n  padding: 0 24dp 40dp 24dp;"));
  EXPECT_EQ(RcssDp(rcss, ".call-phone-hint", "line-height"), 20);
  EXPECT_TRUE(Has(rcss, "margin: 8dp 0 0 0;\n  max-width: 300dp;"));
  EXPECT_EQ(RcssDp(rcss, ".call-phone-control-circle", "height"), 52);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-control", "gap"), 4);
  EXPECT_EQ(RcssDp(rcss, ".call-phone-control-label", "line-height"), 13);
}

// Every step line stays on one line, so a row is always kStepRow tall.
TEST(ShellCallChromeRmlTest, AStepsTextIsOneLineWithAnEllipsis) {
  const std::string rcss = ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss");
  const size_t rule = rcss.find("\n.call-phone-step-text {");
  ASSERT_NE(rule, std::string::npos);
  const std::string body = rcss.substr(rule, rcss.find('}', rule) - rule);
  EXPECT_TRUE(Has(body, "white-space: nowrap;"));
  EXPECT_TRUE(Has(body, "overflow: hidden;"));
  EXPECT_TRUE(Has(body, "text-overflow: ellipsis;"));
}

TEST(ShellCallChromeRmlTest, ThePhoneCallPageShowsTheSilhouetteOnlyForAnUnknownOneToOnePeerWithNoPhoto) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Expanded;
  state.call_in_progress.peer_avatar_letter = "B";
  const std::string known = CallInProgressRml(state);
  EXPECT_FALSE(Has(known, "unknown-caller.svg"));
  EXPECT_TRUE(Has(known, "avatar-letter"));
  state.call_in_progress.peer_unknown = true;
  const std::string unknown = CallInProgressRml(state);
  EXPECT_TRUE(Has(unknown, "call-phone-avatar--unknown avatar-tone-0 call-phone-avatar--big"));
  EXPECT_TRUE(Has(unknown, "unknown-caller.svg"));
  EXPECT_FALSE(Has(unknown, "avatar-letter\">B"));
  state.call_in_progress.peer_has_avatar = true;
  state.call_in_progress.peer_avatar_src = "icons/bob.png";
  EXPECT_FALSE(Has(CallInProgressRml(state), "unknown-caller.svg")) << "a photo wins";
  EXPECT_TRUE(Has(CallInProgressRml(state), "icons/bob.png"));
}

TEST(ShellCallChromeRmlTest, ARosterTileOfAnUnknownMemberShowsTheSilhouetteByABoundFlag) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_in_progress.active = true;
  state.call_in_progress.show_roster = true;
  state.call_in_progress.mode = CallChromeMode::Immersive;
  const std::string rml = CallInProgressRml(state);
  EXPECT_TRUE(Has(rml, "data-if=\"p.avatar_unknown && "));
  EXPECT_TRUE(Has(rml, "unknown-caller.svg"));
  EXPECT_TRUE(Has(rml, "data-if=\"!p.has_avatar && !p.avatar_unknown && "));
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

TEST(ShellCallChromeRmlTest, TheBarsOwnMeterIsLabelledInTheUsersLanguage) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  ShellState state;
  state.call_in_progress.active = true;
  state.call_in_progress.mode = CallChromeMode::Expanded;
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_TRUE(Has(CallInProgressRml(state), "shell-call-meter-label\">" + Tr("call.label.you") + "<"));
  EXPECT_FALSE(Has(CallInProgressRml(state), ">You<"));
  LocalizationService::Instance().SetPreferredLanguage("en");
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
  EXPECT_TRUE(Has(rml,
                  "data-for=\"p : call_in_progress_spotlight_rows\" class=\"call-phone-tile "
                  "call-phone-tile--spotlit\""));
  EXPECT_TRUE(Has(rml, "data-event-dblclick=\"call_spotlight_fit()\""));
  EXPECT_TRUE(Has(rml,
                  "data-for=\"p : call_in_progress_strip_rows\" class=\"call-phone-tile "
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

// #165: the connection story always shows all its steps; which are done (✓), which is current (moving dot) and which are
// plain dots is bound to the story's stage, not written into the markup, so a stage advance dirties and never remounts
// (a remount restarted the pair's spark and the spinner). Same for the 1:1 page and the group card.
std::string StepMarkup(const int i) {
  const std::string stage = "call_in_progress_story_stage";
  return "<div class=\"call-phone-step\" data-class-call-phone-step--done=\"" + stage + " > " + std::to_string(i) +
         "\" data-class-call-phone-step--current=\"" + stage + " == " + std::to_string(i) +
         "\"><div class=\"call-phone-step-dot\"><svg data-if=\"" + stage + " > " + std::to_string(i) +
         "\" src=\"../icons/v2/check.svg\" width=\"12\" height=\"12\"></svg></div><p "
         "class=\"call-phone-step-text\">{{call_in_progress_story_step" +
         std::to_string(i + 1) + "}}</p></div>";
}

TEST(ShellCallChromeRmlTest, PhoneStoryShowsAllStepsAndTheReachedOnesAreBoundToTheStage) {
  for (const bool roster : {false, true}) { // false = 1:1 page, true = group card
    std::string first;
    for (int stage = 0; stage < 4; ++stage) {
      ShellState state = PhoneGroupCall();
      state.call_in_progress.show_roster = roster;
      state.call_in_progress.story_active = true;
      state.call_in_progress.story_stage = stage;
      const std::string rml = CallInProgressRml(state);
      if (stage == 0) {
        first = rml;
      }
      EXPECT_EQ(rml, first) << "a stage advance does not change the markup: roster=" << roster << " stage=" << stage;
      EXPECT_EQ(Count(rml, "<div class=\"call-phone-step-dot\">"), 3u);
      size_t last = 0;
      for (int i = 0; i < 3; ++i) {
        const size_t at = rml.find(StepMarkup(i));
        EXPECT_NE(at, std::string::npos) << "roster=" << roster << " step=" << i + 1;
        EXPECT_GE(at, last); // in order
        last = at;
      }
      EXPECT_EQ(Count(rml, "call-phone-step--current"), 3u) << "only as bound classes";
    }
  }

  // #172: a 1:1 call's five steps: calling, answered, matching, encrypted channel, connected.
  ShellState five = PhoneGroupCall();
  five.call_in_progress.show_roster = false;
  five.call_in_progress.story_active = true;
  five.call_in_progress.story_step_count = 5;
  const std::string rml = CallInProgressRml(five);
  EXPECT_EQ(Count(rml, "<div class=\"call-phone-step-dot\">"), 5u);
  size_t last = 0;
  for (int i = 0; i < 5; ++i) {
    const size_t at = rml.find(StepMarkup(i));
    EXPECT_NE(at, std::string::npos) << "five steps step=" << i + 1;
    EXPECT_GE(at, last);
    last = at;
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

// The rows of the connecting list per kind of call: a 1:1 answerer's three lead rows (done, their words written in)
// plus its four steps = 7; the caller's own five; a group's three; the video card's four, without the lead rows.
TEST(ShellCallChromeRmlTest, PhoneStoryRowsPerKindOfCall) {
  const auto rows = [](const ShellState& state) {
    return Count(CallInProgressRml(state), "<div class=\"call-phone-step-dot\">");
  };
  ShellState answerer = PhoneGroupCall();
  answerer.call_in_progress.show_roster = false;
  answerer.call_in_progress.story_active = true;
  answerer.call_in_progress.story_step_count = 4;
  answerer.call_in_progress.story_lead = {"Call received", "On the encrypted network", "Wang"};
  EXPECT_EQ(rows(answerer), 7u);
  const std::string answerer_rml = CallInProgressRml(answerer);
  EXPECT_LT(answerer_rml.find("Call received"), answerer_rml.find("On the encrypted network"));
  EXPECT_LT(answerer_rml.find("Wang"), answerer_rml.find("{{call_in_progress_story_step1}}"));
  for (const char* lead : {"Call received", "On the encrypted network", "Wang"}) {
    EXPECT_TRUE(Has(answerer_rml, std::string("<div class=\"call-phone-step call-phone-step--done\"><div "
                                              "class=\"call-phone-step-dot\"><svg src=\"../icons/v2/check.svg\" "
                                              "width=\"12\" height=\"12\"></svg></div><p "
                                              "class=\"call-phone-step-text\">") +
                                      lead + "</p>"))
        << lead << " is a done row";
  }

  ShellState caller = answerer;
  caller.call_in_progress.story_lead.clear();
  caller.call_in_progress.story_step_count = 5;
  EXPECT_EQ(rows(caller), 5u);

  ShellState group = answerer;
  group.call_in_progress.show_roster = true;
  group.call_in_progress.story_lead.clear();
  group.call_in_progress.story_step_count = 3;
  EXPECT_EQ(rows(group), 3u);

  ShellState video = answerer; // the lead rows are not carried into the camera card
  video.call_in_progress.stage_visible = true;
  EXPECT_EQ(rows(video), 4u);
  EXPECT_TRUE(Has(CallInProgressRml(video), "call-phone-story call-phone-story--card"));
}

// The callee's page (the answering page of an answered call, and the video card's pair): us first and them second as
// always, the spark running towards us; the caller's page has no such class.
TEST(ShellCallChromeRmlTest, ThePairKeepsUsFirstAndOnlyTheCalleesSparkRunsTowardsUs) {
  for (const bool video_card : {false, true}) {
    ShellState state = PhoneGroupCall();
    state.call_in_progress.show_roster = false;
    state.call_in_progress.story_active = true;
    state.call_in_progress.stage_visible = video_card;
    state.call_in_progress.self_avatar_letter = "S";
    state.call_in_progress.peer_avatar_letter = "P";
    state.call_in_progress.story_inbound = true;
    const std::string callee = CallInProgressRml(state);
    EXPECT_TRUE(Has(callee, "call-phone-pair-path call-phone-pair-path--inbound")) << video_card;
    EXPECT_LT(callee.find("avatar-letter\">S"), callee.find("avatar-letter\">P")) << "us first, " << video_card;
    state.call_in_progress.story_inbound = false;
    const std::string caller = CallInProgressRml(state);
    EXPECT_FALSE(Has(caller, "call-phone-pair-path--inbound")) << video_card;
    EXPECT_TRUE(Has(caller, "class=\"call-phone-pair-path\"")) << video_card;
    EXPECT_LT(caller.find("avatar-letter\">S"), caller.find("avatar-letter\">P")) << "us first, " << video_card;
    EXPECT_TRUE(Has(caller, "call-phone-pair-name call-phone-pair-name--peer\">{{call_in_progress_phone_title}}"));
  }
}

// A list that does not fit moves up inside a clip of fixed height: the clip's height and the list's negative margin
// are written in; with the height unknown nothing is clipped or moved.
TEST(ShellCallChromeRmlTest, ALongListIsClippedAndMovedUpByBoundStyles) {
  ShellState state = PhoneGroupCall();
  state.call_in_progress.show_roster = false;
  state.call_in_progress.story_active = true;
  state.call_in_progress.story_step_count = 4;
  state.call_in_progress.story_lead = {"a", "b", "c"};
  const std::string rml = CallInProgressRml(state);
  EXPECT_TRUE(Has(rml, "<div class=\"call-phone-steps-clip\" data-style-height=\"call_in_progress_story_clip\">"));
  EXPECT_TRUE(Has(rml, "<div class=\"call-phone-steps\" data-style-margin-top=\"call_in_progress_story_offset\">"));
  // The scroll is bound (a stage advance moves the list without a remount): its values are not in the markup.
  state.call_in_progress.story_clip = "158dp"; // five rows: 5 * 34 - 12
  state.call_in_progress.story_offset = "-68dp";
  EXPECT_EQ(CallInProgressRml(state), rml);
  // Without lead rows (a caller's, a group's list) there is no clip and no bound offset at all.
  state.call_in_progress.story_lead.clear();
  EXPECT_FALSE(Has(CallInProgressRml(state), "call-phone-steps-clip"));
  EXPECT_FALSE(Has(CallInProgressRml(state), "data-style-margin-top"));
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
  EXPECT_TRUE(Has(normal,
                  "class=\"call-phone-pip\" id=\"call-local-tile\" tile=\"local\" "
                  "data-if=\"call_in_progress_local_preview\" data-event-click=\"call_spotlight('11')\""));
  EXPECT_TRUE(Has(normal, "class=\"call-phone-remote\" id=\"call-remote-tile\" tile=\"remote\">"));
  EXPECT_TRUE(Has(normal, "<div class=\"call-hold-pill call-hold-pill--who\" data-if=\"call_in_progress_on_hold\">"))
      << "V060: their picture may still run while they are on hold";

  state.call_in_progress.spotlight_stream = "11";
  const std::string swapped = CallInProgressRml(state);
  EXPECT_TRUE(Has(swapped,
                  "<call-video-tile class=\"call-phone-remote\" tile=\"local\" "
                  "data-if=\"call_in_progress_local_preview\">"));
  EXPECT_TRUE(Has(swapped, "tile=\"remote\" data-if=\"!call_in_progress_local_preview\"")) << "camera off: theirs";
  EXPECT_TRUE(Has(swapped,
                  "class=\"call-phone-pip\" tile=\"remote\" "
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
  EXPECT_TRUE(Has(rml,
                  "data-event-click=\"call_flip_camera()\" "
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
  const size_t flip = rml.find(
      "data-event-click=\"call_flip_camera()\" "
      "data-class-call-phone-control--on=\"call_in_progress_camera_front\"");
  ASSERT_NE(flip, std::string::npos);
  EXPECT_LT(rml.find("data-event-click=\"call_camera()\""), flip);
  EXPECT_LT(flip, rml.find("data-event-click=\"call_leave()\""));

  state.call_in_progress.show_flip = false;
  EXPECT_FALSE(Has(CallInProgressRml(state), "call_flip_camera()"));
}

} // namespace
} // namespace pbr
