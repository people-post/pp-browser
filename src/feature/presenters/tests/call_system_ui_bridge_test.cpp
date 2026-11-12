#include "feature/presenters/CallSystemUiBridge.h"

#include "common/Diag.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pbr {
namespace {

/** The system call screen, recording what it was told. */
class FakeCallSystemUi final : public ICallSystemUi {
public:
  bool Available() const override { return available; }
  void RecheckAvailable() override {
    ++rechecks;
    available = available_when_asked_again;
  }
  void SetCallbacks(Callbacks callbacks) override { callbacks_ = std::move(callbacks); }
  bool HasPendingPushCall() const override { return pending_push; }
  void ReportIncoming(const std::string& call_id, const std::string& caller_name, bool video,
                      bool full_screen) override {
    log.push_back("incoming " + call_id + " " + caller_name + (video ? " video" : "") +
                  (full_screen ? "" : " no-full-screen"));
    pending_push = false;
  }
  void ReportEnded(const std::string& call_id) override { log.push_back("ended " + call_id); }

  void NotifyMessagingReady() override { log.push_back("ready"); }
  void EndPendingPushCall() override {
    log.push_back("ended pending");
    pending_push = false;
    if (callbacks_.pending_gone) {
      callbacks_.pending_gone();
    }
  }

  /** What Available() says; RecheckAvailable turns it into what the OS would say now. */
  bool available = true;
  bool available_when_asked_again = true;
  int rechecks = 0;
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
                    .display_name = [](const std::string& identity) { return "name-of-" + identity; },
                    .now_ms = [this]() { return now; },
                    // What CallController's redraw does: the app rings the ring unless the system screen does.
                    // Recorded in the system screen's log, so the order against "ended" shows.
                    .ring_moved =
                        [this]() {
                          ui_.log.push_back((bridge_.SystemRings(ring_id) ? "system rings " : "app rings ") + ring_id);
                          if (on_ring_moved) {
                            on_ring_moved();
                          }
                        },
                    .screen_state = [this]() { return screen; },
                    .pin_locked = [this]() { return pin_locked; },
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
  static CallScreen InCall(const std::string& call_id) {
    CallScreen s;
    s.kind = CallScreen::Kind::InCall;
    s.call_id = call_id;
    return s;
  }

  FakeCallSystemUi ui_;
  std::vector<std::string> did;
  std::vector<std::string> accepted_ids;
  std::function<void()> on_accept;
  std::function<void()> on_ring_moved;
  int64_t now = 1'000'000;
  /** What the platform says about the screen (nothing: the query failed); PP's PIN gate. */
  std::optional<DeviceScreenState> screen = DeviceScreenState{};
  bool pin_locked = false;
  /** The ring ring_moved reports on. */
  std::string ring_id = "c1";
  CallSystemUiBridge bridge_;

  /** The 300 ms ring timer firing for `ms` (Reevaluate at each tick), the app's visibility as given. */
  void TicksFor(const int64_t ms, const bool app_foreground) {
    for (int64_t t = 0; t < ms; t += kTickMs) {
      now += kTickMs;
      bridge_.Reevaluate(app_foreground);
    }
  }
  int Count(const std::string& what) const { return static_cast<int>(std::count(did.begin(), did.end(), what)); }
  static constexpr int64_t kTickMs = 300;
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

// Whether the system screen can ring is asked again for each new ring (the user may have turned call notifications
// off since), not on every frame of a call: on Android asking is a call into the OS, and a call redraws every tick.
TEST_F(CallSystemUiBridgeTest, EachNewRingAsksAgainWhetherTheSystemScreenCanRing) {
  bridge_.Sync(InCall("c0"), "c0", false);
  bridge_.Sync(InCall("c0"), "c0", false);
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_EQ(ui_.rechecks, 0) << "the frames of a call ask nothing again";

  ui_.available_when_asked_again = false; // call notifications turned off meanwhile
  bridge_.Sync(Ring("c1"), "", false);
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_EQ(ui_.rechecks, 1) << "once per ring";
  EXPECT_TRUE(ui_.log.empty()) << "the ring stays in the app";
  EXPECT_FALSE(bridge_.TakesRing(false, false));
  bridge_.Sync(CallScreen{}, "", false);

  ui_.available_when_asked_again = true; // and on again
  bridge_.Sync(Ring("c2"), "", false);
  EXPECT_EQ(ui_.rechecks, 2);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c2 name-of-alice"}));
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
  // Hung up on the system screen.
  ui_.callbacks_.end("c1");
  EXPECT_EQ(did.back(), "leave");
  bridge_.Sync(CallScreen{}, "", false);
  EXPECT_EQ(ui_.log.size(), 1u); // the system screen ended it itself
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
  bridge_.Sync(Answering("c1"), "", true); // he opened the app and tapped Accept
  EXPECT_EQ(ui_.log.back(), "ended c1");
  bridge_.Sync(InCall("c1"), "c1", true);
  EXPECT_EQ(ui_.log.size(), 2u);
}

TEST_F(CallSystemUiBridgeTest, ThePushsCallAnsweredBeforeItsRingShowedAnswersTheRing) {
  ui_.pending_push = true;
  ui_.callbacks_.answer(""); // the app was still waking (or locked)
  now += 10'000;
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_EQ(did, (std::vector<std::string>{"accept voice"}));
  EXPECT_FALSE(bridge_.SystemRings("c1"));
}

// The push's call comes with no call id (the push names none); an id the system screen does not show is stale.
TEST_F(CallSystemUiBridgeTest, AnAnswerForACallNotOnTheSystemScreenIsNoChoice) {
  ui_.pending_push = true;
  ui_.callbacks_.answer("c9");
  ui_.callbacks_.end("c9");
  bridge_.Sync(Ring("c1"), "", false);
  EXPECT_TRUE(did.empty());
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"}));
}

TEST_F(CallSystemUiBridgeTest, ThePushsCallDeclinedBeforeItsRingShowedDeclinesTheRing) {
  ui_.pending_push = true;
  ui_.callbacks_.end("");
  ui_.pending_push = false; // the system screen ended it
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
  ui_.callbacks_.pending_gone(); // timed out or reset: its choice must not hit a later ring
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
  ui_.callbacks_.answer(""); // answered on the system screen; the PIN gate is still up
  now += 100'000;            // longer than kPendingChoiceMs: typing the PIN
  bridge_.OnMessagingReady();
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"ready"}));
  now += 30'000;
  bridge_.Sync(Ring("c1", true), "", false);
  EXPECT_EQ(did, (std::vector<std::string>{"accept"})); // video ring: not voice-only
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
  now += 2'000; // inside the window after the unlock, past the cap since the answer
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
  ui_.callbacks_.end(""); // declined there
  ui_.pending_push = false;
  bridge_.Sync(RingOver("c2", "c1"), "c1", false);
  EXPECT_EQ(did, (std::vector<std::string>{"decline"})) << "declining ends nothing";
  ui_.pending_push = true;
  ui_.callbacks_.end(""); // a second push, its call still on the system screen
  bridge_.Sync(RingOver("c3", "c1"), "c1", false);
  EXPECT_EQ(did, (std::vector<std::string>{"decline", "decline"}));
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"ended pending"}));
}

/** The Diag channel's lines: what Send diagnostics carries (DIAGNOSTICS.md). */
class DiagCapture : public logging::Handler {
public:
  void emit(logging::Level, const std::string&, const std::string& message) override { lines.push_back(message); }
  std::vector<std::string> lines;
};

// #555: the phone's call screen's events are shareable lines — a word and safe numbers, never the call's id.
TEST_F(CallSystemUiBridgeTest, ItsEventsAreShareableDiagLines) {
  auto diag = std::make_shared<DiagCapture>();
  DiagLog().addHandler(diag);
  ui_.pending_push = true;
  ui_.callbacks_.answer("");
  bridge_.Sync(Ring("c1"), "", false);
  ASSERT_EQ(did, (std::vector<std::string>{"accept voice"}));
  ASSERT_FALSE(diag->lines.empty());
  EXPECT_NE(diag->lines.front().find("event=call.system what=answer_pending"), std::string::npos)
      << diag->lines.front();
  for (const std::string& line : diag->lines) {
    EXPECT_EQ(line.find("c1"), std::string::npos) << line;
  }
}

TEST_F(CallSystemUiBridgeTest, ACallOnTheSystemScreenStaysThereWhileASecondRingsInTheApp) {
  // The first call was answered on the system screen; a second call rings.
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
  EXPECT_EQ(accepted_ids, (std::vector<std::string>{"c1"}));
}

TEST_F(CallSystemUiBridgeTest, NothingWithoutASystemScreen) {
  ICallSystemUi none;
  CallSystemUiBridge bridge(none, {});
  bridge.Sync(Ring("c1"), "", false);
  EXPECT_FALSE(bridge.SystemRings("c1"));
}

// --- Handing the ring back to the app (DEV/PLAN-android-incoming-call-screen.md) ---------------------------------

// MIUI raises the app on the notification's full-screen intent: once the user has seen the app for 300 ms, its own ring
// page and ringtone take the ring, then the notification goes — never told to the call as a decline or an end.
TEST_F(CallSystemUiBridgeTest, SeenForTheSettleTimeTheRingComesBackToTheAppOnce) {
  bridge_.Sync(Ring("c1"), "", /*app_foreground=*/false);
  ASSERT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"})) << "the first one raises the app";
  EXPECT_TRUE(bridge_.WantsReevaluation()) << "the 300 ms re-evaluation runs while the system screen rings";

  bridge_.Sync(Ring("c1"), "", true); // MIUI raised the app: a frame alone hands nothing back
  bridge_.Reevaluate(true);           // the foreground listener: the settle time starts
  now += kCallRingHandBackSettleMs - 1;
  bridge_.Reevaluate(true);
  EXPECT_TRUE(bridge_.SystemRings("c1")) << "not yet 300 ms";
  now += 1;
  bridge_.Reevaluate(true);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice", "app rings c1", "ended c1"}))
      << "the app's ring page and ringtone first, then the notification goes";
  EXPECT_FALSE(bridge_.SystemRings("c1"));
  EXPECT_FALSE(bridge_.WantsReevaluation()) << "handed back: the re-evaluation stops";
  EXPECT_EQ(Count("decline"), 0) << "taking the notification away is not a decline";
  EXPECT_EQ(Count("leave"), 0) << "nor the end of the call";

  TicksFor(3'000, true);
  bridge_.Sync(Ring("c1"), "", true);
  EXPECT_EQ(ui_.log.size(), 3u) << "once";
  EXPECT_TRUE(did.empty());
}

TEST_F(CallSystemUiBridgeTest, ARedrawThatEndsTheRingDuringTheHandBackIsNotReportedTwice) {
  bridge_.Sync(Ring("c1"), "", false);
  bridge_.Reevaluate(true);
  // The caller hung up just as the app redraws: that frame lets the notification go itself.
  on_ring_moved = [this]() { bridge_.Sync(CallScreen{}, "", true); };
  now += kTickMs;
  bridge_.Reevaluate(true);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice", "app rings c1", "ended c1"}));
  EXPECT_FALSE(bridge_.WantsReevaluation());
  EXPECT_TRUE(did.empty());
}

TEST_F(CallSystemUiBridgeTest, ALockedOrDarkPhoneInFrontKeepsTheRingOnTheSystemScreen) {
  for (const auto& [name, state] : std::vector<std::pair<std::string, std::optional<DeviceScreenState>>>{
           {"locked", DeviceScreenState{.interactive = true, .keyguard_locked = true}},
           {"dark", DeviceScreenState{.interactive = false, .keyguard_locked = false}},
           {"query failed", std::nullopt},
       }) {
    const std::string id = "call-" + name; // a new call each round
    ui_.log.clear();
    screen = state;
    bridge_.Sync(Ring(id), "", false);
    TicksFor(3'000, true);
    EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming " + id + " name-of-alice"})) << name;
    EXPECT_TRUE(bridge_.SystemRings(id)) << name << ": the notification and its ringtone stay";
    EXPECT_TRUE(bridge_.WantsReevaluation()) << name;
    bridge_.Sync(CallScreen{}, "", false); // the caller gives up
    EXPECT_EQ(ui_.log.back(), "ended " + id) << name;
    EXPECT_FALSE(bridge_.WantsReevaluation()) << name << ": the timer stops with the ring";
  }
}

// Seen on a locked or dark phone, an app in front is not seen: the ring goes to the system screen even then.
TEST_F(CallSystemUiBridgeTest, ARingOnALockedPhoneGoesToTheSystemScreenEvenWithTheAppInFront) {
  screen = DeviceScreenState{.interactive = true, .keyguard_locked = true};
  EXPECT_TRUE(bridge_.TakesRing(true, false));
  screen = std::nullopt;
  EXPECT_TRUE(bridge_.TakesRing(true, false)) << "a failed query counts as not seen";
  screen = DeviceScreenState{};
  pin_locked = true;
  EXPECT_TRUE(bridge_.TakesRing(true, false)) << "behind the PIN";
  pin_locked = false;
  EXPECT_FALSE(bridge_.TakesRing(true, false));
}

TEST_F(CallSystemUiBridgeTest, BackToTheBackgroundBeforeTheSettleTimeHandsNothingBack) {
  bridge_.Sync(Ring("c1"), "", false);
  bridge_.Reevaluate(true);
  now += 200;
  bridge_.Reevaluate(false); // MIUI let the app go again
  now += 200;
  bridge_.Reevaluate(true); // in front again: the time starts over
  now += kCallRingHandBackSettleMs - 1;
  bridge_.Reevaluate(true);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"}));
  EXPECT_TRUE(bridge_.SystemRings("c1"));
}

// Not only the foreground switch counts: unlocking the phone with the app already in front is seen by the timer.
TEST_F(CallSystemUiBridgeTest, LockedInFrontThenUnlockedTheTimerHandsTheRingBack) {
  screen = DeviceScreenState{.interactive = true, .keyguard_locked = true};
  bridge_.Sync(Ring("c1"), "", false);
  bridge_.Reevaluate(true); // the foreground listener: behind the keyguard
  TicksFor(1'500, true);
  ASSERT_TRUE(bridge_.SystemRings("c1"));
  screen = DeviceScreenState{}; // unlocked; no lifecycle event comes
  now += kTickMs;
  bridge_.Reevaluate(true); // the first tick that sees it
  EXPECT_TRUE(bridge_.SystemRings("c1"));
  now += kTickMs;
  bridge_.Reevaluate(true); // 300 ms later
  EXPECT_FALSE(bridge_.SystemRings("c1"));
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice", "app rings c1", "ended c1"}));
}

TEST_F(CallSystemUiBridgeTest, BehindThePinTheRingStaysThereUntilThePinIsEntered) {
  pin_locked = true;
  bridge_.Sync(Ring("c1"), "", false);
  bridge_.Reevaluate(true); // raised onto the PIN page
  TicksFor(3'000, true);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"})) << "no ring page over the PIN";
  pin_locked = false; // the PIN is entered
  TicksFor(kTickMs, true);
  EXPECT_TRUE(bridge_.SystemRings("c1"));
  TicksFor(kTickMs, true);
  EXPECT_FALSE(bridge_.SystemRings("c1")) << "300 ms after the PIN";
  EXPECT_EQ(ui_.log.back(), "ended c1");
}

TEST_F(CallSystemUiBridgeTest, AnsweredOnTheNotificationOrWithAPushWaitingNothingIsHandedBack) {
  bridge_.Sync(Ring("c1"), "", false);
  ui_.pending_push = true; // a second call's push waits for its ring
  TicksFor(3'000, true);
  EXPECT_TRUE(bridge_.SystemRings("c1"));
  ui_.pending_push = false;
  ui_.callbacks_.answer("c1");
  EXPECT_FALSE(bridge_.WantsReevaluation()) << "answered there: it keeps the call";
  TicksFor(3'000, true);
  EXPECT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice"}));
  EXPECT_EQ(did, (std::vector<std::string>{"accept voice"}));
}

// Given back, then the user leaves the app while it still rings: the system screen rings it again, but this
// notification must not raise the app (no full-screen intent), or Home would pull the app straight back up.
TEST_F(CallSystemUiBridgeTest, LeftWhileTheAppRingsItTheNotificationReturnsWithoutRaisingTheApp) {
  struct Leave {
    std::string name;
    bool app_foreground;
    std::optional<DeviceScreenState> screen;
    bool pin_locked;
  };
  const std::vector<Leave> leaves = {
      {"Home", false, DeviceScreenState{}, false},
      {"screen off", true, DeviceScreenState{.interactive = false, .keyguard_locked = false}, false},
      {"locked", true, DeviceScreenState{.interactive = true, .keyguard_locked = true}, false},
      {"PIN", true, DeviceScreenState{}, true},
  };
  int n = 0;
  for (const Leave& leave : leaves) {
    ring_id = "c" + std::to_string(++n); // a new call each round
    ui_.log.clear();
    screen = DeviceScreenState{};
    pin_locked = false;
    bridge_.Sync(Ring(ring_id), "", false);
    bridge_.Reevaluate(true);
    TicksFor(kTickMs, true);
    ASSERT_EQ(ui_.log, (std::vector<std::string>{"incoming " + ring_id + " name-of-alice", "app rings " + ring_id,
                                                 "ended " + ring_id}))
        << leave.name;

    screen = leave.screen;
    pin_locked = leave.pin_locked;
    bridge_.Reevaluate(leave.app_foreground); // the next tick sees the app gone
    EXPECT_TRUE(bridge_.SystemRings(ring_id)) << leave.name;
    ASSERT_EQ(ui_.log.size(), 5u) << leave.name;
    EXPECT_EQ(ui_.log[3], "incoming " + ring_id + " name-of-alice no-full-screen") << leave.name;
    EXPECT_EQ(ui_.log[4], "system rings " + ring_id) << leave.name << ": the app's page and ringtone go";
    EXPECT_TRUE(bridge_.WantsReevaluation()) << leave.name;
    TicksFor(3'000, leave.app_foreground);
    EXPECT_EQ(ui_.log.size(), 5u) << leave.name << ": it stays on the system screen meanwhile";

    // Seen again: back to the app after 300 ms, still without any decline.
    screen = DeviceScreenState{};
    pin_locked = false;
    TicksFor(2 * kTickMs, true);
    EXPECT_FALSE(bridge_.SystemRings(ring_id)) << leave.name;
    EXPECT_EQ(ui_.log.back(), "ended " + ring_id) << leave.name;
    EXPECT_TRUE(did.empty()) << leave.name;
    bridge_.Sync(CallScreen{}, "", true); // over
  }
}

// The app rang it in front from the start (no notification yet), then the user pressed Home: the notification that
// takes it must not raise the app either — the user has seen this call already.
TEST_F(CallSystemUiBridgeTest, RungInFrontFromTheStartThenLeftTheNotificationDoesNotRaiseTheApp) {
  bridge_.Sync(Ring("c1"), "", /*app_foreground=*/true);
  TicksFor(3 * kTickMs, true);
  ASSERT_TRUE(ui_.log.empty()) << "the app rings it";

  bridge_.Reevaluate(false); // Home
  ASSERT_EQ(ui_.log, (std::vector<std::string>{"incoming c1 name-of-alice no-full-screen", "system rings c1"}));
  EXPECT_TRUE(bridge_.SystemRings("c1"));
  EXPECT_TRUE(did.empty());
}

TEST_F(CallSystemUiBridgeTest, ANewCallsFirstNotificationRaisesTheAppAgain) {
  bridge_.Sync(Ring("c1"), "", false);
  TicksFor(2 * kTickMs, true);
  bridge_.Reevaluate(false);
  bridge_.Sync(CallScreen{}, "", false);
  bridge_.Sync(Ring("c2"), "", false);
  EXPECT_EQ(ui_.log.back(), "incoming c2 name-of-alice");
}

TEST_F(CallSystemUiBridgeTest, ARingOverACallIsNeverTakenByTheReevaluation) {
  bridge_.Sync(InCall("c1"), "c1", true);
  bridge_.Sync(RingOver("c2", "c1"), "c1", true);
  TicksFor(1'000, false);
  EXPECT_TRUE(ui_.log.empty());
}

TEST_F(CallSystemUiBridgeTest, TheRingEndingClearsTheStateAndStopsTheReevaluation) {
  bridge_.Sync(Ring("c1"), "", false);
  bridge_.Reevaluate(true);
  bridge_.Sync(CallScreen{}, "", true);
  EXPECT_EQ(ui_.log.back(), "ended c1");
  EXPECT_FALSE(bridge_.WantsReevaluation());
  TicksFor(1'000, true);
  TicksFor(1'000, false);
  EXPECT_EQ(ui_.log.size(), 2u) << "nothing rings, nothing is handed anywhere";
  EXPECT_TRUE(did.empty());
}

} // namespace
} // namespace pbr
