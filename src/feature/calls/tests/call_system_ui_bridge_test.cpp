#include "feature/calls/CallSystemUiBridge.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

/** The system call screen, recording what it was told. */
class FakeCallSystemUi final : public ICallSystemUi {
public:
  bool Available() const override { return true; }
  void SetCallbacks(Callbacks callbacks) override { callbacks_ = std::move(callbacks); }
  bool HasPendingPushCall() const override { return pending_push; }
  void ReportIncoming(const std::string& call_id, const std::string& caller_name, bool video) override {
    log.push_back("incoming " + call_id + " " + caller_name + (video ? " video" : ""));
    pending_push = false;
  }
  void ReportEnded(const std::string& call_id, EndReason reason) override {
    log.push_back("ended " + call_id + (reason == EndReason::AnsweredElsewhere ? " answered-elsewhere" : ""));
  }

  bool pending_push = false;
  std::vector<std::string> log;
  Callbacks callbacks_;
};

class CallSystemUiBridgeTest : public ::testing::Test {
protected:
  CallSystemUiBridgeTest()
      : bridge_(ui_, {
                         .accept = [this](bool voice_only) { did.push_back(voice_only ? "accept voice" : "accept"); },
                         .decline = [this]() { did.push_back("decline"); },
                         .leave = [this]() { did.push_back("leave"); },
                         .toggle_mute = [this]() { did.push_back("toggle mute"); },
                         .display_name = [](const std::string& identity) { return "name-of-" + identity; },
                         .now_ms = [this]() { return now; },
                     }) {}

  static CallScreen Ring(const std::string& call_id, bool video = false) {
    CallScreen s;
    s.kind = CallScreen::Kind::Ringing;
    s.ring_call_id = call_id;
    s.caller_identity = "alice";
    s.ring_video = video;
    return s;
  }
  static CallScreen Answering(const std::string& call_id) {
    CallScreen s = Ring(call_id);
    s.kind = CallScreen::Kind::Answering;
    return s;
  }
  static CallScreen InCall(const std::string& call_id, bool muted = false) {
    CallScreen s;
    s.kind = CallScreen::Kind::InCall;
    s.call_id = call_id;
    s.muted = muted;
    return s;
  }

  FakeCallSystemUi ui_;
  std::vector<std::string> did;
  int64_t now = 1'000'000;
  CallSystemUiBridge bridge_;
};

TEST_F(CallSystemUiBridgeTest, ARingInTheForegroundStaysInTheApp) {
  EXPECT_FALSE(bridge_.TakesRing(true));
  EXPECT_TRUE(bridge_.TakesRing(false));
  bridge_.Sync(Ring("c1"), "", /*app_foreground=*/true);
  EXPECT_TRUE(ui_.log.empty());
  EXPECT_FALSE(bridge_.SystemRings("c1"));
}

TEST_F(CallSystemUiBridgeTest, ARingInTheBackgroundRingsOnTheSystemScreenAndEndsThere) {
  bridge_.Sync(Ring("c1", true), "", false);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice video"}));
  EXPECT_TRUE(bridge_.SystemRings("c1"));
  // The caller gives up: the ring goes.
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_EQ(ui_.log.back(), "ended c1");
  EXPECT_FALSE(bridge_.SystemRings("c1"));
}

TEST_F(CallSystemUiBridgeTest, ThePushsCallIsFilledInEvenWithTheAppOpen) {
  ui_.pending_push = true;
  bridge_.Sync(Ring("c1"), "", true);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"}));
}

TEST_F(CallSystemUiBridgeTest, AnsweredOnTheSystemScreenItKeepsTheCallUntilItEnds) {
  bridge_.Sync(Ring("c1", true), "", false);
  ui_.callbacks_.answer("c1");
  EXPECT_EQ(did, (std::vector<std::string>{"accept"}));
  EXPECT_FALSE(bridge_.SystemRings("c1"));
  bridge_.Sync(Answering("c1"), "", false);
  bridge_.Sync(InCall("c1"), "c1", false);
  EXPECT_EQ(ui_.log.size(), 1u);
  // Muted on the system screen.
  ui_.callbacks_.set_muted("c1", true);
  EXPECT_EQ(did.back(), "toggle mute");
  bridge_.Sync(InCall("c1", true), "c1", false);
  ui_.callbacks_.set_muted("c1", true);  // already muted
  EXPECT_EQ(did.size(), 2u);
  // Hung up on the system screen.
  ui_.callbacks_.end("c1");
  EXPECT_EQ(did.back(), "leave");
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_EQ(ui_.log.size(), 1u);  // the system screen ended it itself
}

TEST_F(CallSystemUiBridgeTest, ThePeerHangingUpEndsItOnTheSystemScreen) {
  bridge_.Sync(Ring("c1"), "", false);
  ui_.callbacks_.answer("c1");
  bridge_.Sync(InCall("c1"), "c1", false);
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_EQ(ui_.log.back(), "ended c1");
}

TEST_F(CallSystemUiBridgeTest, DeclinedOnTheSystemScreen) {
  bridge_.Sync(Ring("c1"), "", false);
  ui_.callbacks_.end("c1");
  EXPECT_EQ(did, (std::vector<std::string>{"decline"}));
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_EQ(ui_.log.size(), 1u);
}

TEST_F(CallSystemUiBridgeTest, AnsweredInTheAppTheSystemScreenLetsItGo) {
  bridge_.Sync(Ring("c1"), "", false);
  bridge_.Sync(Answering("c1"), "", true);  // he opened the app and tapped Accept
  EXPECT_EQ(ui_.log.back(), "ended c1 answered-elsewhere");
  bridge_.Sync(InCall("c1"), "c1", true);
  EXPECT_EQ(ui_.log.size(), 2u);
}

TEST_F(CallSystemUiBridgeTest, ThePushsCallAnsweredBeforeItsRingShowedAnswersTheRing) {
  ui_.pending_push = true;
  ui_.callbacks_.answer("");  // the app was still waking (or locked)
  now += 10'000;
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_EQ(did, (std::vector<std::string>{"accept voice"}));
  EXPECT_FALSE(bridge_.SystemRings("c1"));
}

TEST_F(CallSystemUiBridgeTest, ThePushsCallDeclinedBeforeItsRingShowedDeclinesTheRing) {
  ui_.pending_push = true;
  ui_.callbacks_.end("");
  ui_.pending_push = false;  // the system screen ended it
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_EQ(did, (std::vector<std::string>{"decline"}));
  EXPECT_TRUE(ui_.log.empty());
}

TEST_F(CallSystemUiBridgeTest, AChoiceOnThePushsCallExpires) {
  ui_.callbacks_.end("");
  now += CallSystemUiBridge::kPendingChoiceMs + 1;
  bridge_.Sync(Ring("c2"), "", false);
  EXPECT_TRUE(did.empty());
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c2 name-of-alice"}));
}

TEST_F(CallSystemUiBridgeTest, NothingWithoutASystemScreen) {
  ICallSystemUi none;
  CallSystemUiBridge bridge(none, {});
  bridge.Sync(Ring("c1"), "", false);
  EXPECT_FALSE(bridge.SystemRings("c1"));
}

} // namespace
} // namespace pbr
