#include "feature/presenters/CallSystemUiBridge.h"

#include <gtest/gtest.h>

#include <functional>
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

  void NotifyMessagingReady() override { log.push_back("ready"); }
  void EndPendingPushCall() override {
    log.push_back("ended pending");
    pending_push = false;
    if (callbacks_.pending_gone) {
      callbacks_.pending_gone();
    }
  }

  bool pending_push = false;
  std::vector<std::string> log;
  Callbacks callbacks_;
};

class CallSystemUiBridgeTest : public ::testing::Test {
protected:
  CallSystemUiBridgeTest()
      : bridge_(ui_,
                {
                    .accept =
                        [this](const std::string& call_id, bool voice_only) {
                          did.push_back(voice_only ? "accept voice" : "accept");
                          accepted_ids.push_back(call_id);
                          if (on_accept) {
                            on_accept(); // what the presenter does: it redraws at once
                          }
                        },
                    .decline = [this]() { did.push_back("decline"); },
                    .leave = [this]() { did.push_back("leave"); },
                    .toggle_mute = [this]() { did.push_back("toggle mute"); },
                    .display_name = [](const std::string& identity) { return "name-of-" + identity; },
                    .now_ms = [this]() { return now; },
                    .audio_managed = [this](bool managed) { audio.push_back(managed ? "managed" : "unmanaged"); },
                    .audio_activated = [this](bool active) { audio.push_back(active ? "activated" : "deactivated"); },
                }) {}

  static CallScreen Ring(const std::string& call_id, bool video = false) {
    CallScreen s;
    s.kind = CallScreen::Kind::Ringing;
    s.ring_call_id = call_id;
    s.caller_identity = "alice";
    s.ring_video = video;
    return s;
  }
  /** `call_id` rings over the call `ending` this device is in (answering ends it). */
  static CallScreen RingOver(const std::string& call_id, const std::string& ending) {
    CallScreen s = Ring(call_id);
    s.conflict = true;
    s.ending.call_id = ending;
    s.ending.peer_identity = "bob";
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
  std::vector<std::string> accepted_ids;
  /** What AudioSession was told (managed by the system screen, its activation). */
  std::vector<std::string> audio;
  std::function<void()> on_accept;
  int64_t now = 1'000'000;
  CallSystemUiBridge bridge_;
};

TEST_F(CallSystemUiBridgeTest, ARingInTheForegroundStaysInTheApp) {
  EXPECT_FALSE(bridge_.TakesRing(true, false));
  EXPECT_TRUE(bridge_.TakesRing(false, false));
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

TEST_F(CallSystemUiBridgeTest, AnsweredOnTheSystemScreenItsAudioIsTheSystemsUntilTheCallEnds) {
  bridge_.Sync(Ring("c1"), "", false);
  on_accept = [this]() { EXPECT_EQ(audio, (std::vector<std::string>{"managed"})) << "before the accept's audio"; };
  ui_.callbacks_.answer("c1");
  ASSERT_EQ(did, (std::vector<std::string>{"accept voice"}));
  ui_.callbacks_.audio_session(true); // CallKit's didActivateAudioSession
  bridge_.Sync(InCall("c1"), "c1", false);
  EXPECT_EQ(audio, (std::vector<std::string>{"managed", "activated"}));
  bridge_.Sync(CallScreen{}, "", false); // the peer hung up
  EXPECT_EQ(audio.back(), "unmanaged");
}

TEST_F(CallSystemUiBridgeTest, TheSystemDeactivatingTheSessionEndsItsHold) {
  bridge_.Sync(Ring("c1"), "", false);
  ui_.callbacks_.answer("c1");
  ui_.callbacks_.end("c1"); // hung up there: CallKit releases the session and says so
  ui_.callbacks_.audio_session(false);
  EXPECT_EQ(audio, (std::vector<std::string>{"managed", "deactivated", "unmanaged"}));
}

TEST_F(CallSystemUiBridgeTest, AnsweredInTheAppTheAudioStaysTheApps) {
  bridge_.Sync(Ring("c1"), "", false);
  bridge_.Sync(Answering("c1"), "", true);
  bridge_.Sync(InCall("c1"), "c1", true);
  EXPECT_TRUE(audio.empty());
}

TEST_F(CallSystemUiBridgeTest, ThePushsAnswerHoldsTheAudioOnlyOnceItsRingShows) {
  ui_.pending_push = true;
  ui_.callbacks_.answer("");
  EXPECT_TRUE(audio.empty()) << "a placeholder that never finds its ring must leave nothing managed";
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_EQ(audio, (std::vector<std::string>{"managed"}));
}

TEST_F(CallSystemUiBridgeTest, ThePushsCallGoingAwayReleasesTheAudio) {
  ui_.pending_push = true;
  ui_.callbacks_.answer("");
  ui_.callbacks_.audio_session(true); // CallKit activated it on the answer
  ui_.pending_push = false;
  ui_.callbacks_.pending_gone();
  EXPECT_EQ(audio, (std::vector<std::string>{"activated", "unmanaged"}));
}

// P016: the VoIP push names its call (`wake_hint`); a choice made on the push's call is that call's only.
TEST_F(CallSystemUiBridgeTest, AnAnswerOnThePushsNamedCallAnswersOnlyThatRing) {
  ui_.pending_push = true;
  ui_.callbacks_.answer("c1");         // the push named c1; its ring is not known yet
  bridge_.Sync(Ring("c2"), "", false); // another call rings first
  EXPECT_TRUE(did.empty()) << "c2 is not the call the user answered";
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c2 name-of-alice"}));
  EXPECT_TRUE(audio.empty());
  bridge_.Sync(CallScreen{}, "", false); // c2's caller gives up
  EXPECT_EQ(ui_.log.back(), "ended c2");
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_EQ(did, (std::vector<std::string>{"accept voice"}));
  EXPECT_EQ(accepted_ids, (std::vector<std::string>{"c1"}));
  EXPECT_EQ(ui_.log.back(), "incoming c1 name-of-alice");
}

TEST_F(CallSystemUiBridgeTest, ADeclineOnThePushsNamedCallDeclinesOnlyThatRing) {
  ui_.pending_push = true;
  ui_.callbacks_.end("c1");
  ui_.pending_push = false; // the system screen ended the push's call
  bridge_.Sync(Ring("c2"), "", false);
  EXPECT_TRUE(did.empty());
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c2 name-of-alice"}));
  ui_.callbacks_.end("c2"); // declined there too
  EXPECT_EQ(did, (std::vector<std::string>{"decline"}));
  bridge_.Sync(CallScreen{}, "", false);
  // The user opened the app meanwhile: the call he declined there does not ring in it.
  bridge_.Sync(Ring("c1"), "", true);
  EXPECT_EQ(did, (std::vector<std::string>{"decline", "decline"}));
  EXPECT_EQ(ui_.log.size(), 1u);
}

TEST_F(CallSystemUiBridgeTest, AChoiceOnThePushsNamedCallStillExpires) {
  ui_.callbacks_.end("c1");
  now += CallSystemUiBridge::kPendingChoiceMs + 1;
  bridge_.Sync(Ring("c1"), "", true);
  EXPECT_TRUE(did.empty());
}

TEST_F(CallSystemUiBridgeTest, ThePushsNamedCallGoingAwayForgetsItsAnswer) {
  ui_.pending_push = true;
  ui_.callbacks_.answer("c1");
  ui_.pending_push = false;
  ui_.callbacks_.pending_gone(); // its wait ran out
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_TRUE(did.empty());
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

// The bug answered calls had: Accept redraws inside the system screen's answer (CallPresenter::Accept, Changed),
// so Sync re-enters before the app shows the call. That frame must not end the call just answered there.
TEST_F(CallSystemUiBridgeTest, AnAcceptThatRedrawsAtOnceKeepsTheCallOnTheSystemScreen) {
  bridge_.Sync(Ring("c1"), "", false);
  on_accept = [this]() { bridge_.Sync(Answering("c1"), "", false); }; // the presenter's click frame
  ui_.callbacks_.answer("c1");
  EXPECT_EQ(accepted_ids, (std::vector<std::string>{"c1"})) << "the call the system screen answered";
  bridge_.Sync(InCall("c1"), "c1", false);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"})) << "nothing ended";
}

TEST_F(CallSystemUiBridgeTest, AFrameWithoutTheCallRightAfterTheAnswerDoesNotEndIt) {
  bridge_.Sync(Ring("c1"), "", false);
  on_accept = [this]() { bridge_.Sync(CallScreen{}, "", false); }; // an empty frame before "answering"
  ui_.callbacks_.answer("c1");
  EXPECT_EQ(ui_.log.size(), 1u) << "the call stays while the app takes it";
  bridge_.Sync(Answering("c1"), "", false);
  bridge_.Sync(InCall("c1"), "c1", false);
  EXPECT_EQ(ui_.log.size(), 1u);
}

TEST_F(CallSystemUiBridgeTest, AnAnswerTheAppNeverTakesEndsOnTheSystemScreenInTime) {
  bridge_.Sync(Ring("c1"), "", false);
  ui_.callbacks_.answer("c1");
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_EQ(ui_.log.size(), 1u);
  now += CallSystemUiBridge::kAnswerSettleMs;
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_EQ(ui_.log.back(), "ended c1") << "the accept failed: the system screen lets it go";
}

TEST_F(CallSystemUiBridgeTest, ThePushsAnswerAppliedAtTheRingAlsoSurvivesTheRedraw) {
  ui_.pending_push = true;
  ui_.callbacks_.answer(""); // answered before the ring was known
  on_accept = [this]() { bridge_.Sync(Answering("c1"), "", true); };
  bridge_.Sync(Ring("c1"), "", true);
  EXPECT_EQ(accepted_ids, (std::vector<std::string>{"c1"}));
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"}));
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

TEST_F(CallSystemUiBridgeTest, AJoinAnsweredByTheAppEndsThePushsCallAtOnce) {
  ui_.pending_push = true;
  bridge_.Sync(Answering("c1"), "", false);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"ended pending"}));
  EXPECT_FALSE(ui_.pending_push);
}

TEST_F(CallSystemUiBridgeTest, AWaitingPushCallIsLeftAloneWhileNoCallIsUp) {
  ui_.pending_push = true;
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_TRUE(ui_.log.empty());
  EXPECT_TRUE(ui_.pending_push);
}

TEST_F(CallSystemUiBridgeTest, ThePushsCallGoingAwayForgetsAChoiceMadeOnIt) {
  ui_.pending_push = true;
  ui_.callbacks_.answer("");
  ui_.pending_push = false;
  ui_.callbacks_.pending_gone();  // timed out or reset: its choice must not hit a later ring
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_TRUE(did.empty());
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"}));
}

TEST_F(CallSystemUiBridgeTest, AChoiceOnThePushsCallExpires) {
  ui_.callbacks_.end("");
  now += CallSystemUiBridge::kPendingChoiceMs + 1;
  bridge_.Sync(Ring("c2"), "", false);
  EXPECT_TRUE(did.empty());
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c2 name-of-alice"}));
}

TEST_F(CallSystemUiBridgeTest, AnsweredWhileLockedTheWindowCountsFromTheUnlock) {
  ui_.pending_push = true;
  ui_.callbacks_.answer("");  // CallKit answered; the PIN gate is still up
  now += 100'000;            // longer than kPendingChoiceMs: typing the PIN
  bridge_.OnMessagingReady();
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"ready"}));
  now += 30'000;
  bridge_.Sync(Ring("c1", true), "", false);
  EXPECT_EQ(did, (std::vector<std::string>{"accept"}));  // video ring: not voice-only
  EXPECT_FALSE(bridge_.SystemRings("c1"));
}

TEST_F(CallSystemUiBridgeTest, AnsweredWhileLockedTheRingMustShowWithinTheWindowAfterTheUnlock) {
  ui_.callbacks_.answer("");
  now += 100'000;
  bridge_.OnMessagingReady();
  now += CallSystemUiBridge::kPendingChoiceMs + 1;
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_TRUE(did.empty());
  EXPECT_EQ(ui_.log.back(), "incoming c1 name-of-alice");
}

TEST_F(CallSystemUiBridgeTest, AChoiceMadeAfterTheUnlockCountsFromTheChoice) {
  bridge_.OnMessagingReady();
  now += 500'000;
  ui_.callbacks_.answer("");
  now += CallSystemUiBridge::kPendingChoiceMs - 1;
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_EQ(did, (std::vector<std::string>{"accept voice"}));
}

TEST_F(CallSystemUiBridgeTest, AnAnswerWhileLockedNeverOutlivesTheAbsoluteCap) {
  ui_.callbacks_.answer("");
  now += CallSystemUiBridge::kPendingMaxMs - 1'000;
  bridge_.OnMessagingReady();
  now += 2'000;  // inside the window after the unlock, past the cap since the answer
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_TRUE(did.empty());
}

TEST_F(CallSystemUiBridgeTest, DeclinedWhileLockedDeclinesTheRingOnceFound) {
  ui_.pending_push = true;
  ui_.callbacks_.end("");
  ui_.pending_push = false;
  now += 100'000;
  bridge_.OnMessagingReady();
  now += 5'000;
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_EQ(did, (std::vector<std::string>{"decline"}));
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"ready"}));
}

// V064: answering a ring over a call ends that call. The system screen's Answer does not say so; the app's
// ring (End & accept) and its alert do — so such a ring never goes to the system screen.
TEST_F(CallSystemUiBridgeTest, ARingOverACallInTheBackgroundStaysInTheApp) {
  bridge_.Sync(InCall("c1"), "c1", false); // answered in the app, then the app went to the background
  EXPECT_FALSE(bridge_.TakesRing(false, /*over_call=*/true)) << "its alert is the app's, worded for the conflict";
  bridge_.Sync(RingOver("c2", "c1"), "c1", false);
  EXPECT_TRUE(ui_.log.empty());
  EXPECT_FALSE(bridge_.SystemRings("c2")) << "the app rings it";
  EXPECT_TRUE(did.empty()) << "nothing ends the call the user is in";
}

TEST_F(CallSystemUiBridgeTest, AnAnswerOnThePushsCallIsNotAppliedToARingOverACall) {
  bridge_.Sync(InCall("c1"), "c1", false);
  ui_.pending_push = true;
  ui_.callbacks_.answer(""); // "Incoming call" answered before its ring showed: nothing said it ends c1
  bridge_.Sync(RingOver("c2", "c1"), "c1", false);
  EXPECT_TRUE(did.empty()) << "the app's ring asks first (End & accept)";
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"ended pending"})) << "the push's call gives way to the app's ring";
  EXPECT_FALSE(bridge_.SystemRings("c2"));
  // Spent: the same ring, once the first call is gone, is not answered by that earlier tap.
  bridge_.Sync(Ring("c2"), "", false);
  EXPECT_TRUE(did.empty());
}

TEST_F(CallSystemUiBridgeTest, ADeclineOnThePushsCallStillDeclinesARingOverACall) {
  bridge_.Sync(InCall("c1"), "c1", false);
  ui_.pending_push = true;
  ui_.callbacks_.end("c2"); // the push named c2; declined there
  ui_.pending_push = false;
  bridge_.Sync(RingOver("c2", "c1"), "c1", false);
  EXPECT_EQ(did, (std::vector<std::string>{"decline"})) << "declining ends nothing";
  ui_.pending_push = true;
  ui_.callbacks_.end(""); // a push that named no call
  bridge_.Sync(RingOver("c3", "c1"), "c1", false);
  EXPECT_EQ(did, (std::vector<std::string>{"decline", "decline"}));
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"ended pending"}));
}

TEST_F(CallSystemUiBridgeTest, ACallOnTheSystemScreenStaysThereWhileASecondRingsInTheApp) {
  // iPhone: the first call was answered on CallKit; a second call rings.
  bridge_.Sync(Ring("c1"), "", false);
  ui_.callbacks_.answer("c1");
  bridge_.Sync(InCall("c1"), "c1", false);
  ASSERT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"}));
  bridge_.Sync(RingOver("c2", "c1"), "c1", false);
  EXPECT_EQ(ui_.log.size(), 1u) << "c1 stays on the system screen; c2 is not reported there";
  EXPECT_FALSE(bridge_.SystemRings("c2"));
  // End & accept in the app: c1 goes, and the system screen lets it go.
  bridge_.Sync(Answering("c2"), "", false);
  EXPECT_EQ(ui_.log.back(), "ended c1");
  EXPECT_EQ(audio.back(), "unmanaged");
  EXPECT_EQ(accepted_ids, (std::vector<std::string>{"c1"}));
}

TEST_F(CallSystemUiBridgeTest, TheSystemsAudioSessionCallbacksAreAccepted) {
  ui_.callbacks_.audio_session(true);
  ui_.callbacks_.audio_session(false);
  EXPECT_TRUE(did.empty());
  EXPECT_EQ(audio, (std::vector<std::string>{"activated", "deactivated", "unmanaged"}));
}

TEST_F(CallSystemUiBridgeTest, NothingWithoutASystemScreen) {
  ICallSystemUi none;
  CallSystemUiBridge bridge(none, {});
  bridge.Sync(Ring("c1"), "", false);
  EXPECT_FALSE(bridge.SystemRings("c1"));
}

} // namespace
} // namespace pbr
