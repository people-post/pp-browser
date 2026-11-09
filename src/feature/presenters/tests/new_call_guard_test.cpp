// TODO #191: a call button pressed while a call is up asks before it ends that call; the same chat just shows it.
#include "feature/presenters/NewCallGuard.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

CallScreen InCallIn(const std::string& thread_id) {
  CallScreen s;
  s.kind = CallScreen::Kind::InCall;
  s.call_id = "c1";
  s.thread_id = thread_id;
  return s;
}

/** Records what ran; `answer` is what the user taps (none: the dialog stays open). */
struct Rig {
  std::vector<std::string> log;
  std::function<void(bool)> pending;
  NewCallSteps Steps() {
    NewCallSteps steps;
    steps.ask = [this](std::function<void(bool)> answer) {
      log.push_back("ask");
      pending = std::move(answer);
    };
    steps.show_current = [this]() { log.push_back("show"); };
    steps.leave_current = [this]() { log.push_back("leave"); };
    steps.place = [this]() { log.push_back("place"); };
    steps.cancelled = [this]() { log.push_back("cancelled"); };
    return steps;
  }
};

TEST(NewCallGuardTest, NoCallPlacesWithoutAsking) {
  EXPECT_EQ(DecideNewCall(CallScreen{}, "dm-bob"), NewCallVerdict::Place);
  Rig rig;
  RunNewCall(NewCallVerdict::Place, rig.Steps());
  EXPECT_EQ(rig.log, (std::vector<std::string>{"place"}));
}

TEST(NewCallGuardTest, AnotherChatAsksFirstAndNothingHappensUntilAnswered) {
  EXPECT_EQ(DecideNewCall(InCallIn("dm-bob"), "dm-carol"), NewCallVerdict::AskThenReplace);
  Rig rig;
  RunNewCall(NewCallVerdict::AskThenReplace, rig.Steps());
  EXPECT_EQ(rig.log, (std::vector<std::string>{"ask"}));
}

TEST(NewCallGuardTest, ConfirmLeavesThenPlaces) {
  Rig rig;
  RunNewCall(NewCallVerdict::AskThenReplace, rig.Steps());
  rig.pending(true);
  EXPECT_EQ(rig.log, (std::vector<std::string>{"ask", "leave", "place"}));
}

TEST(NewCallGuardTest, CancelDoesNothingToTheCall) {
  Rig rig;
  RunNewCall(NewCallVerdict::AskThenReplace, rig.Steps());
  rig.pending(false);
  EXPECT_EQ(rig.log, (std::vector<std::string>{"ask", "cancelled"}));
}

TEST(NewCallGuardTest, SameChatShowsTheCallWithoutAsking) {
  EXPECT_EQ(DecideNewCall(InCallIn("grp-1"), "grp-1"), NewCallVerdict::ShowCurrent);
  Rig rig;
  RunNewCall(NewCallVerdict::ShowCurrent, rig.Steps());
  EXPECT_EQ(rig.log, (std::vector<std::string>{"show"}));
}

TEST(NewCallGuardTest, AGroupCallAskedAboutLikeAOneToOne) {
  EXPECT_EQ(DecideNewCall(InCallIn("grp-1"), "dm-bob"), NewCallVerdict::AskThenReplace);
}

TEST(NewCallGuardTest, ARingOrAnswerOnScreenIsNotACallUp) {
  CallScreen ring;
  ring.kind = CallScreen::Kind::Ringing;
  EXPECT_EQ(DecideNewCall(ring, "dm-bob"), NewCallVerdict::Place);
}

TEST(NewCallGuardTest, NoDialogAvailableCancelsRatherThanEndingTheCall) {
  Rig rig;
  NewCallSteps steps = rig.Steps();
  steps.ask = nullptr;
  RunNewCall(NewCallVerdict::AskThenReplace, steps);
  EXPECT_EQ(rig.log, (std::vector<std::string>{"cancelled"}));
}

} // namespace
} // namespace pbr
