#include "feature/presenters/CallLocalHold.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

/**
 * A call's media as the hold sees it: the mic, the camera, and the call's answers to "tell them we are on hold". The
 * camera models the real interface (B225): requests are asynchronous (they land at `Pump()`), `SetLocalVideoPaused(true)`
 * does nothing when the camera is already closed and then leaves no "paused" mark, `SetLocalVideoEnabled` opens or
 * closes unconditionally and clears every "paused", and each effective change is one roster.
 */
struct FakeCall {
  bool muted = false;
  /** The engine's camera, as the device really is. */
  bool engine_camera = false;
  bool paused_here = false;         // video_paused_call_id_
  bool roster_video_paused = false; // what the others read
  /** Publishes are answered when the test says (SetLocalOnHold replies later, on UI). */
  std::vector<std::pair<bool, std::function<void(Roe<void>)>>> publishes;
  std::vector<bool> told;
  /** B221: every roster that would leave, in order: what the others see of {muted, on_hold} after it. */
  std::vector<std::pair<bool, bool>> rosters; // {muted, on_hold}
  bool on_hold_row = false;
  std::vector<std::string> notes;

  /** The camera requests in flight, in order. */
  struct Request {
    bool pause = false;
    bool enable = false;
    std::function<void(Roe<void>)> done;
  };
  std::vector<Request> queue;
  /** What was asked and when (`now` at the request): 'E' = enable, 'D' = disable, 'P' = pause. */
  std::vector<std::pair<int64_t, char>> requests;
  int camera_rosters = 0;
  int device_opens = 0;
  int open_failed = 0;
  int64_t now = 0;
  bool drop_requests = false; // the engine never applies them
  bool refuse_enable = false;

  CallLocalHold::Ports Ports() {
    CallLocalHold::Ports ports;
    ports.mic_muted = [this]() { return muted; };
    ports.engine_camera_on = [this]() { return engine_camera; };
    ports.publish_hold = [this](bool on_hold, std::optional<bool> mute, std::function<void(Roe<void>)> done) {
      if (mute) {
        muted = *mute; // the call applies the mic change and the hold, then sends ONE roster
      }
      on_hold_row = on_hold;
      rosters.emplace_back(muted, on_hold_row);
      publishes.emplace_back(on_hold, std::move(done));
    };
    ports.set_mic_muted = [this](bool m) {
      muted = m; // SetLocalAudioMuted: its own roster, with the hold as the store has it now
      rosters.emplace_back(muted, on_hold_row);
    };
    ports.set_camera_enabled = [this](bool enabled, std::function<void(Roe<void>)> done) {
      requests.emplace_back(now, enabled ? 'E' : 'D');
      queue.push_back({false, enabled, std::move(done)});
    };
    ports.pause_camera = [this]() {
      requests.emplace_back(now, 'P');
      queue.push_back({true, false, {}});
    };
    ports.camera_open_failed = [this]() { ++open_failed; };
    ports.note = [this](const std::string& key) { notes.push_back(key); };
    return ports;
  }
  /** The engine works through the camera requests (CallSessionManager on its own thread). */
  void Pump() {
    auto pending = std::move(queue);
    queue.clear();
    if (drop_requests) {
      return;
    }
    for (auto& request : pending) {
      if (request.pause) {
        if (!engine_camera) {
          continue; // CallSessionManager.cpp:229: nothing to pause, and no paused mark
        }
        engine_camera = false;
        paused_here = true;
        roster_video_paused = true;
        ++camera_rosters;
      } else if (request.enable && refuse_enable) {
        if (request.done) {
          request.done(Error("Video is not allowed for this call"));
        }
      } else {
        if (request.enable && !engine_camera) {
          ++device_opens;
        }
        engine_camera = request.enable;
        paused_here = false;
        roster_video_paused = false;
        ++camera_rosters;
        if (request.done) {
          request.done({});
        }
      }
    }
  }
  int Count(char kind) const {
    return static_cast<int>(
        std::count_if(requests.begin(), requests.end(), [kind](const auto& r) { return r.second == kind; }));
  }
  /** The call has media: the oldest publish is told. */
  void Tell() {
    ASSERT_FALSE(publishes.empty());
    auto [on_hold, done] = std::move(publishes.front());
    publishes.erase(publishes.begin());
    told.push_back(on_hold);
    done({});
  }
  /** Still ringing out: the oldest publish fails as SetLocalOnHold does without call media. */
  void Refuse() {
    ASSERT_FALSE(publishes.empty());
    auto done = std::move(publishes.front().second);
    publishes.erase(publishes.begin());
    done(Error("No active call media"));
  }
};

/** The monotonic clock is `now_ms` too unless a test says otherwise (the wall clock then differs). */
CallLocalHold::Facts In(const std::string& call_id, bool audio_taken, int64_t now_ms, bool foreground = true,
                        bool visible = true) {
  return {.call_id = call_id,
          .audio_taken = audio_taken,
          .foreground = foreground,
          .visible = visible,
          .now_ms = now_ms,
          .mono_ms = now_ms};
}

/** One call "c1" in progress from t = 0, with the camera's clock helpers. */
struct Rig {
  FakeCall call;
  CallLocalHold hold;
  bool audio = false;
  Rig() : hold(call.Ports()) { hold.Sync(In("c1", false, 0)); }
  /** A UI tick at `t`; the engine lands what was asked before it (unless `pump` is false). */
  void Tick(int64_t t, bool pump = true) {
    call.now = t;
    if (pump) {
      call.Pump();
    }
    hold.Sync(In("c1", audio, t));
  }
  /** Ticks every 100 ms up to and including `to`. */
  void Run(int64_t from, int64_t to) {
    for (int64_t t = from; t <= to; t += 100) {
      Tick(t);
    }
  }
  void Tap(int64_t t) {
    call.now = t;
    hold.OnCameraTapped(t);
  }
  void Auto(int64_t t) {
    call.now = t;
    hold.OnAutoCameraOn(t);
  }
  void Bg(int64_t t) {
    call.now = t;
    hold.OnEnteredBackground(t);
  }
  void WillFg() { hold.OnWillEnterForeground(); }
  void Inactive(int64_t t) {
    call.now = t;
    hold.OnInactive();
  }
  void Active(int64_t t) {
    call.now = t;
    hold.OnActive(t);
  }
  /** The camera is on and landed (a tap at t, the engine pumped). */
  void CameraOn(int64_t t = 0) {
    Tap(t);
    call.Pump();
    ASSERT_TRUE(call.engine_camera);
  }
};

// V060: the OS takes the call's audio — the others are told it is on hold, and told again when it ends.
TEST(CallLocalHoldTest, TheOthersAreToldWhenTheHoldBeginsAndEnds) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", false, 0));
  EXPECT_TRUE(call.publishes.empty()) << "no hold, nothing to tell";
  hold.Sync(In("c1", true, 10));
  EXPECT_TRUE(hold.OnHold());
  ASSERT_EQ(call.publishes.size(), 1u);
  call.Tell();
  hold.Sync(In("c1", true, 20));
  EXPECT_TRUE(call.publishes.empty()) << "told once";
  hold.Sync(In("c1", false, 30));
  EXPECT_FALSE(hold.OnHold());
  ASSERT_EQ(call.publishes.size(), 1u);
  call.Tell();
  EXPECT_EQ(call.told, (std::vector<bool>{true, false}));
  EXPECT_EQ(call.notes, (std::vector<std::string>{"call.status.on_hold_local", "call.status.on_hold_ended"}));
}

// B221: the end of a hold reaches the others as one roster carrying the final state of both fields; never the stale
// pair (mic open, still on hold) that could land last and keep the peer on "away".
TEST(CallLocalHoldTest, TheEndOfAHoldIsOneRosterWithTheFinalState) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", false, 0));
  hold.Sync(In("c1", true, 10));
  ASSERT_EQ(call.rosters.size(), 1u) << "start: mute + hold in one roster";
  EXPECT_EQ(call.rosters.back(), (std::pair<bool, bool>{true, true}));
  call.Tell();
  call.rosters.clear();
  hold.Sync(In("c1", false, 30));
  ASSERT_EQ(call.rosters.size(), 1u) << "end: mic open + hold over in one roster";
  EXPECT_EQ(call.rosters.back(), (std::pair<bool, bool>{false, false}));
  EXPECT_FALSE(call.muted);
  call.Tell();
  for (const auto& r : call.rosters) {
    EXPECT_FALSE(r.second) << "no roster after the end says on_hold";
    EXPECT_FALSE(r.first == false && r.second == true);
  }
}

// The user muted before the hold: the hold changes nothing about the mic, and the end stays muted.
TEST(CallLocalHoldTest, AMuteTheUserMadeBeforeTheHoldStaysThroughTheEnd) {
  FakeCall call;
  call.muted = true;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 10));
  call.Tell();
  hold.Sync(In("c1", false, 30));
  call.Tell();
  EXPECT_TRUE(call.muted);
  EXPECT_EQ(call.told, (std::vector<bool>{true, false}));
  for (const auto& r : call.rosters) {
    EXPECT_TRUE(r.first);
  }
  EXPECT_FALSE(call.rosters.back().second);
}

// The end rides on the publish; if that publish fails (no media yet) the retry still tells the hold.
TEST(CallLocalHoldTest, AFailedPublishWithTheMuteIsStillRetried) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 0));
  call.Refuse();
  hold.Sync(In("c1", true, CallLocalHold::kPublishRetryMs));
  ASSERT_EQ(call.publishes.size(), 1u);
  call.Tell();
  EXPECT_EQ(call.told, std::vector<bool>{true});
}

// A hold that begins while the call rings out (no call media yet) is told once the media is up — not lost.
TEST(CallLocalHoldTest, AHoldTheCallCouldNotTakeYetIsToldOnceItCan) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 0));
  call.Refuse();
  hold.Sync(In("c1", true, 500));
  EXPECT_TRUE(call.publishes.empty()) << "at most once a second";
  hold.Sync(In("c1", true, CallLocalHold::kPublishRetryMs));
  ASSERT_EQ(call.publishes.size(), 1u) << "sent again";
  call.Refuse();
  hold.Sync(In("c1", true, 2 * CallLocalHold::kPublishRetryMs));
  ASSERT_EQ(call.publishes.size(), 1u);
  call.Tell(); // the call connected
  EXPECT_EQ(call.told, std::vector<bool>{true});
  hold.Sync(In("c1", true, 5 * CallLocalHold::kPublishRetryMs));
  EXPECT_TRUE(call.publishes.empty()) << "told: no more sends";
}

// The hold ends before the call could ever be told: there is nothing to say.
TEST(CallLocalHoldTest, AHoldThatEndsBeforeItWasToldSaysNothing) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 0));
  call.Refuse();
  hold.Sync(In("c1", false, 100));
  hold.Sync(In("c1", false, 5000));
  EXPECT_TRUE(call.publishes.empty());
  EXPECT_TRUE(call.told.empty());
}

// The hold ends while its own publish is in flight: once that lands, the end is told at once.
TEST(CallLocalHoldTest, AnEndDuringTheHoldsPublishIsToldRightAfter) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 0));
  hold.Sync(In("c1", false, 10));
  EXPECT_EQ(call.publishes.size(), 1u) << "one at a time";
  call.Tell();
  hold.Sync(In("c1", false, 20));
  ASSERT_EQ(call.publishes.size(), 1u);
  EXPECT_FALSE(call.publishes.front().first);
  call.Tell();
  EXPECT_EQ(call.told, (std::vector<bool>{true, false}));
}

// A hold going on into the next call tells that call too; an answer about the earlier call changes nothing.
TEST(CallLocalHoldTest, EachCallIsToldOnItsOwn) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 0));
  hold.Sync(In("c2", true, 10));
  ASSERT_EQ(call.publishes.size(), 2u) << "c2 is told without waiting for c1's answer";
  call.Tell();   // c1's answer: about the old call
  call.Refuse(); // c2's: not yet
  hold.Sync(In("c2", true, 10 + CallLocalHold::kPublishRetryMs));
  ASSERT_EQ(call.publishes.size(), 1u) << "c2 still owed";
}

// Privacy: the hold mutes an open mic and its end opens it again; a mute the user made before stays.
TEST(CallLocalHoldTest, TheHoldMutesAnOpenMicAndKeepsTheUsersOwnMute) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 0));
  EXPECT_TRUE(call.muted);
  hold.Sync(In("c1", false, 10));
  EXPECT_FALSE(call.muted);

  call.muted = true; // the user's own mute
  hold.Sync(In("c1", true, 20));
  hold.Sync(In("c1", false, 30));
  EXPECT_TRUE(call.muted) << "the hold's end does not unmute the user";
}

// Unmuting by hand during the hold (any path, once the hold's mute was seen applied) is the user's choice.
TEST(CallLocalHoldTest, AnUnmuteByHandDuringTheHoldStays) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 0));
  hold.Sync(In("c1", true, 10)); // the hold's mute seen applied
  call.muted = false;            // unmuted elsewhere (the phone's call screen)
  hold.Sync(In("c1", true, 20));
  call.muted = true; // and muted again by hand
  hold.Sync(In("c1", false, 30));
  EXPECT_TRUE(call.muted) << "the end of the hold left the user's choice";

  FakeCall tapped;
  CallLocalHold tap_hold(tapped.Ports());
  tap_hold.Sync(In("c1", true, 0));
  tap_hold.OnMuteTapped();
  tapped.muted = false; // the tap's toggle
  tap_hold.Sync(In("c1", false, 10));
  EXPECT_FALSE(tapped.muted);
  EXPECT_EQ(tapped.notes, std::vector<std::string>{"call.status.on_hold_local"})
      << "no 'mic back' note: the user did it";
}

// ---- the camera (B225) ----------------------------------------------------------------------------------------------

// The hold rests a camera that is on, and its end brings it back — through the gate, not at once.
TEST(CallLocalHoldTest, TheCameraRestsDuringTheHold) {
  Rig r;
  r.CameraOn(0);
  r.audio = true;
  r.Tick(1000);
  EXPECT_EQ(r.call.Count('P'), 1);
  r.call.Pump();
  EXPECT_FALSE(r.call.engine_camera);
  EXPECT_FALSE(r.hold.WantsCameraOn()) << "the button shows off while the camera only rests for the hold";
  r.Active(1100); // an "active" meanwhile does not bring it back
  r.Run(1200, 1900);
  EXPECT_EQ(r.call.Count('E'), 1) << "on hold: nothing opens";
  r.audio = false;
  r.Tick(2000); // the hold ends 1 s after the camera rested: the open waits for the gate
  EXPECT_EQ(r.call.Count('E'), 1);
  EXPECT_TRUE(r.hold.WantsCameraOn()) << "the rest is over: the button shows the wish again";
  EXPECT_TRUE(r.hold.CameraResuming());
  r.Run(2100, 3400);
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(3500);
  EXPECT_EQ(r.call.Count('E'), 2) << "kCameraSettleMs after our close";
}

// While the camera rests for the hold the button shows off; a tap there takes the rest off and brings the camera back,
// like any tap on an off button; it passes the gate.
TEST(CallLocalHoldTest, ACameraTapDuringTheHoldBringsTheCameraBack) {
  Rig r;
  r.CameraOn(0);
  EXPECT_TRUE(r.hold.WantsCameraOn());
  r.audio = true;
  r.Tick(1000);
  EXPECT_FALSE(r.hold.WantsCameraOn()) << "the button shows off during the rest";
  r.Tap(1200);
  EXPECT_TRUE(r.hold.WantsCameraOn()) << "and on after the tap";
  EXPECT_EQ(r.call.Count('D'), 0) << "the first tap during a hold does not turn the wish off";
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(1300);
  EXPECT_EQ(r.call.Count('E'), 1) << "the gate: our rest closed it 1.2 s ago";
  r.Tick(3500);
  EXPECT_EQ(r.call.Count('E'), 2);
  r.call.Pump();
  EXPECT_TRUE(r.call.engine_camera);
  r.Tap(4000); // from here the next tap is the button's: off
  EXPECT_FALSE(r.hold.WantsCameraOn());
  r.audio = false;
  r.Tick(4100);
  r.Tick(9000);
  EXPECT_EQ(r.call.Count('E'), 2) << "the hold's end has nothing to undo";
}

// The button is off during the hold's rest, whatever the wish: with the camera off before the hold, a tap turns it on.
TEST(CallLocalHoldTest, TheButtonIsOffDuringTheHoldRest) {
  Rig r;
  r.CameraOn(0);
  r.audio = true;
  r.Run(100, 1000);
  EXPECT_FALSE(r.hold.WantsCameraOn());
  EXPECT_FALSE(r.hold.CameraResuming()) << "resting for the hold is not 'restoring'";
  r.call.Pump();

  Rig off; // the camera was off when the hold began
  off.audio = true;
  off.Tick(1000);
  EXPECT_FALSE(off.hold.WantsCameraOn());
  off.Tap(1200);
  EXPECT_TRUE(off.hold.WantsCameraOn()) << "off shown, tap turns it on";
  EXPECT_EQ(off.call.Count('E'), 1);
  off.Tap(1300);
  EXPECT_FALSE(off.hold.WantsCameraOn()) << "the next tap is the button's: off";
}

// Mobile: a real background rests a camera that is on; with the camera off there is nothing to rest.
TEST(CallLocalHoldTest, TheBackgroundRestsTheCamera) {
  Rig off;
  off.Bg(100);
  EXPECT_TRUE(off.call.requests.empty()) << "camera off: nothing to rest";

  Rig r;
  r.CameraOn(0);
  r.Bg(100);
  EXPECT_EQ(r.call.Count('P'), 1);
  r.call.Pump();
  EXPECT_FALSE(r.call.engine_camera);
  EXPECT_TRUE(r.call.roster_video_paused);
}

// B225 in the new model: the lock screen's inactive -> active -> inactive within ~0.1 s moves nothing.
TEST(CallLocalHoldTest, FlickerOfInactiveAndActiveMovesNothing) {
  Rig r;
  r.CameraOn(0);
  const auto requests = r.call.requests.size();
  const int rosters = r.call.camera_rosters;
  r.Inactive(1000);
  r.call.Pump();
  r.Active(1120);
  r.Inactive(1128);
  r.call.Pump();
  r.Run(1200, 8000);
  r.Active(8100);
  r.Inactive(8200); // a second inactive inside the (no) settle
  r.Active(8300);
  r.Run(8400, 20000);
  EXPECT_EQ(r.call.requests.size(), requests) << "no request";
  EXPECT_EQ(r.call.camera_rosters, rosters) << "no roster";
  EXPECT_TRUE(r.call.engine_camera);
}

// The same sequence with a real DID_ENTER_BACKGROUND in it: exactly one pause, and never an open afterwards.
TEST(CallLocalHoldTest, TheB225SequenceWithARealBackgroundPausesOnceAndNeverOpens) {
  Rig r;
  r.CameraOn(0);
  const int opens = r.call.device_opens;
  r.Inactive(1000);
  r.Bg(1010); // the callback pauses at once
  r.call.Pump();
  r.Active(1120); // the lone active the lock screen sends
  r.Inactive(1128);
  r.call.Pump();
  r.Run(1200, 61000);
  EXPECT_EQ(r.call.Count('P'), 1);
  EXPECT_EQ(r.call.Count('E'), 1) << "no open after the first";
  EXPECT_EQ(r.call.device_opens, opens);
  EXPECT_FALSE(r.call.engine_camera);
}

// A lone "active" after a real background does not restore; a real return plus 2.5 s of staying active does.
TEST(CallLocalHoldTest, ALoneActiveAfterARealBackgroundDoesNotRestore) {
  Rig lone;
  lone.CameraOn(0);
  lone.Bg(1000);
  lone.call.Pump();
  lone.Active(1100);
  lone.Run(1200, 61100);
  EXPECT_EQ(lone.call.Count('E'), 1) << "no open without a real return";
  EXPECT_FALSE(lone.call.engine_camera);

  Rig real;
  real.CameraOn(0);
  real.Bg(1000);
  real.call.Pump();
  real.WillFg();
  real.Active(2000);
  real.Run(2100, 4400);
  EXPECT_EQ(real.call.Count('E'), 1);
  EXPECT_TRUE(real.hold.CameraResuming());
  real.Tick(4500);
  EXPECT_EQ(real.call.Count('E'), 2) << "exactly one open, 2.5 s after the app became active";
  real.Run(4600, 9000);
  EXPECT_EQ(real.call.Count('E'), 2);
  EXPECT_TRUE(real.call.engine_camera);
  EXPECT_FALSE(real.hold.CameraResuming());
}

// Inactive inside the settle postpones it (the settle restarts at the next active); a real background cancels it.
TEST(CallLocalHoldTest, InactiveInsideTheSettlePostponesIt) {
  Rig r;
  r.CameraOn(0);
  r.Bg(1000);
  r.call.Pump();
  r.WillFg();
  r.Active(2000); // settled at 4500
  r.Inactive(3000);
  r.Run(3100, 5000);
  EXPECT_EQ(r.call.Count('E'), 1) << "inactive: the settle is off";
  r.Active(5100); // settled at 7600
  r.Run(5200, 7500);
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(7600);
  EXPECT_EQ(r.call.Count('E'), 2);
}

// The way out when WILL_ENTER_FOREGROUND never arrives: the user taps the button while the app is active.
TEST(CallLocalHoldTest, ATapWithNoRealReturnSeenClearsTheBackgroundMarkButStillWaits) {
  Rig r;
  r.Bg(1000); // camera off: wants_ false
  r.Active(1100);
  r.Tap(1600);
  EXPECT_TRUE(r.hold.WantsCameraOn());
  EXPECT_TRUE(r.hold.CameraResuming());
  r.Run(1700, 4000);
  EXPECT_EQ(r.call.requests.size(), 0u) << "2.49 s after the tap: nothing";
  r.Tick(4100);
  EXPECT_EQ(r.call.Count('E'), 1) << "2.5 s after the tap, exactly one open";

  // Sooner than the gate: our own close sets the later of the two times.
  Rig g;
  g.CameraOn(0);
  g.Bg(1000);
  g.call.Pump();
  g.Active(1100);
  g.Tap(1200); // the button says on, the app is active, no real return seen: "resume", not "off"
  EXPECT_TRUE(g.hold.WantsCameraOn());
  EXPECT_EQ(g.call.Count('D'), 0);
  g.Run(1300, 3600);
  EXPECT_EQ(g.call.Count('E'), 1) << "before tap + 2.5 s";
  g.Tick(3700);
  EXPECT_EQ(g.call.Count('E'), 2);
  g.call.Pump();
  EXPECT_TRUE(g.call.engine_camera);
  g.Tap(4000); // settled again: now the tap is "off"
  EXPECT_FALSE(g.hold.WantsCameraOn());
}

// A tap while the app is not active (a banner over it) is no resume.
TEST(CallLocalHoldTest, ATapWhileInactiveIsNotAResume) {
  Rig r;
  r.CameraOn(0);
  r.Bg(1000);
  r.call.Pump();
  r.Inactive(1100);
  r.Tap(1200);
  EXPECT_FALSE(r.hold.WantsCameraOn()) << "no active app, no resume signal: the tap is the button's";
}

// The engine closed the camera itself before our pause arrived: the pause is an empty no-op (no paused mark), yet the
// way back is an unconditional open, and the wish never left.
TEST(CallLocalHoldTest, AnEngineThatClosedFirstIsStillReopenedAfterTheRealReturn) {
  Rig r;
  r.CameraOn(0);
  r.call.engine_camera = false; // closed by the engine, silently
  r.Bg(500);
  EXPECT_TRUE(r.hold.WantsCameraOn());
  r.call.Pump();
  EXPECT_FALSE(r.call.paused_here) << "the pause found nothing to pause";
  r.WillFg();
  r.Active(1000);
  r.Run(1100, 3400);
  EXPECT_TRUE(r.hold.WantsCameraOn());
  EXPECT_FALSE(r.call.engine_camera);
  r.Tick(3500);
  r.call.Pump();
  EXPECT_TRUE(r.call.engine_camera) << "an unconditional SetLocalVideoEnabled(true): the camera really reopened";
  EXPECT_TRUE(r.hold.WantsCameraOn());
}

// The request is asynchronous: a tick that still reads the old engine state changes nothing and asks nothing twice.
TEST(CallLocalHoldTest, ARequestInFlightIsNotReadBackAsOff) {
  Rig r;
  r.Tap(0);
  r.Tick(100, /*pump=*/false);
  r.Tick(200, /*pump=*/false);
  EXPECT_TRUE(r.hold.WantsCameraOn());
  EXPECT_EQ(r.call.Count('E'), 1) << "no second request";
  r.call.Pump();
  EXPECT_TRUE(r.call.engine_camera);

  // 3 s later it still does not match: the engine closed it by itself — in an active, settled app the wish follows.
  Rig stuck;
  stuck.call.drop_requests = true;
  stuck.Tap(0);
  stuck.Run(100, 2900);
  EXPECT_TRUE(stuck.hold.WantsCameraOn());
  stuck.Tick(3000);
  EXPECT_FALSE(stuck.hold.WantsCameraOn());

  // Inactive (a permission prompt, Control Center): the same read-back changes nothing.
  Rig inactive;
  inactive.call.drop_requests = true;
  inactive.Tap(0);
  inactive.Inactive(100);
  inactive.Run(200, 9000);
  EXPECT_TRUE(inactive.hold.WantsCameraOn());
}

// The read-back safety net (a camera the engine closed without a word) does not gate the next open; an engine-side close
// that was reported (OnCameraClosedByEngine) does, see below.
TEST(CallLocalHoldTest, AnEngineSideCloseDoesNotGateTheNextOpen) {
  Rig r;
  r.CameraOn(0);
  r.call.engine_camera = false; // silently closed
  r.Run(100, 2900);
  EXPECT_TRUE(r.hold.WantsCameraOn());
  r.Tick(3000);
  EXPECT_FALSE(r.hold.WantsCameraOn()) << "the button reads off";
  r.Tap(3100);
  EXPECT_EQ(r.call.Count('E'), 2) << "opened at once";
  EXPECT_EQ(r.call.requests.back().first, 3100);
  EXPECT_FALSE(r.hold.CameraResuming());
}

// A call answered under the lock screen: the automatic camera-on is the user's intent, and no device opens until a
// real return has settled.
TEST(CallLocalHoldTest, AnAutoCameraUnderTheLockScreenWaitsForTheRealReturn) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", false, 0, /*foreground=*/false, /*visible=*/false));
  call.now = 100;
  hold.OnAutoCameraOn(100);
  EXPECT_TRUE(hold.WantsCameraOn());
  for (int64_t t = 200; t <= 60000; t += 100) {
    call.now = t;
    call.Pump();
    hold.Sync(In("c1", false, t, false, false));
  }
  EXPECT_TRUE(call.requests.empty()) << "no open request in the background";
  hold.OnWillEnterForeground();
  call.now = 61000;
  hold.OnActive(61000);
  for (int64_t t = 61100; t < 63500; t += 100) {
    call.now = t;
    hold.Sync(In("c1", false, t));
  }
  EXPECT_TRUE(call.requests.empty()) << "2.4 s after the return";
  call.now = 63500;
  hold.Sync(In("c1", false, 63500));
  EXPECT_EQ(call.Count('E'), 1);
  EXPECT_EQ(call.requests.size(), 1u);
}

// The first Sync before the asynchronous auto-on landed must not turn the wish into "off"; a tap then turns it off.
TEST(CallLocalHoldTest, TheAutoIntentIsNotOverwrittenByAReadBack) {
  Rig r;
  r.Auto(0);
  r.Tick(100, /*pump=*/false); // the request has not landed: the engine is still off
  EXPECT_TRUE(r.hold.WantsCameraOn());
  r.Tap(200); // the button is live: this tap is "off"
  EXPECT_FALSE(r.hold.WantsCameraOn());
  EXPECT_EQ(r.call.Count('D'), 1);
}

// Real background, back, real background again inside the settle: no open at all, no device opened in the background.
TEST(CallLocalHoldTest, ABackgroundDuringTheSettleNeverOpens) {
  Rig r;
  r.CameraOn(0);
  r.Bg(1000);
  r.call.Pump();
  r.WillFg();
  r.Active(2000);
  r.Bg(3000); // inside the settle
  r.call.Pump();
  r.Run(3100, 30000);
  EXPECT_EQ(r.call.Count('E'), 1);
  EXPECT_EQ(r.call.device_opens, 1);
  EXPECT_FALSE(r.call.engine_camera);
}

// One roster per change that took effect; a postponed or cancelled resume says nothing.
TEST(CallLocalHoldTest, EachEffectiveChangeIsOneRoster) {
  Rig r;
  r.CameraOn(0);
  EXPECT_EQ(r.call.camera_rosters, 1);
  r.Bg(1000);
  r.call.Pump();
  EXPECT_EQ(r.call.camera_rosters, 2) << "the pause";
  r.WillFg();
  r.Active(2000);
  r.Inactive(2500); // postponed
  r.Active(2600);
  r.Run(2700, 4000);
  EXPECT_EQ(r.call.camera_rosters, 2);
  r.Run(4100, 5200);
  EXPECT_EQ(r.call.camera_rosters, 3) << "the open";
  EXPECT_FALSE(r.call.roster_video_paused);
}

// The 2.5 s gate only holds an open back.
TEST(CallLocalHoldTest, AnOpenWaitsForTheGateAfterOurClose) {
  Rig r;
  r.CameraOn(0);
  r.Tap(100); // off
  r.Tick(200);
  r.Tap(2500); // 2.4 s after our close
  EXPECT_TRUE(r.hold.WantsCameraOn());
  EXPECT_TRUE(r.hold.CameraResuming()) << "the button says on, the tile says restoring";
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(2599);
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(2600);
  EXPECT_EQ(r.call.Count('E'), 2) << "exactly one";
  r.Run(2700, 5000);
  EXPECT_EQ(r.call.Count('E'), 2);

  // A tap off while waiting cancels it; a second tap on goes out once, at the gate.
  Rig w;
  w.CameraOn(0);
  w.Tap(100);
  const auto before = w.call.requests.size();
  w.Tap(2500);
  w.Tap(2550); // off while waiting
  EXPECT_EQ(w.call.requests.size(), before) << "nothing to cancel on the wire";
  w.Tap(2560);
  w.Run(2600, 6000);
  EXPECT_EQ(w.call.Count('E'), 2);

  // The first open of a voice call is not held.
  Rig v;
  v.Tap(10);
  EXPECT_EQ(v.call.Count('E'), 1);
  EXPECT_EQ(v.call.requests.front().first, 10);
}

// A close is immediate, however recently the camera opened.
TEST(CallLocalHoldTest, ACloseTakesEffectAtOnce) {
  Rig r;
  r.CameraOn(0);
  r.Tap(1000);
  EXPECT_EQ(r.call.Count('D'), 1);
  EXPECT_EQ(r.call.requests.back().first, 1000);
  EXPECT_FALSE(r.hold.WantsCameraOn());
  EXPECT_FALSE(r.hold.CameraResuming());
  r.call.Pump();
  EXPECT_FALSE(r.call.engine_camera);
  r.Run(1100, 9000);
  EXPECT_EQ(r.call.requests.size(), 2u) << "no delayed request";
}

// Open, close at 0.5 s, open at 1 s: the close goes out when tapped, the open at 2.5 s after it.
TEST(CallLocalHoldTest, FastOpenCloseOpen) {
  Rig r;
  r.Tap(0);
  r.call.Pump();
  r.Tap(500);
  r.call.Pump();
  r.Tap(1000);
  r.Run(1100, 2900);
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(3000);
  EXPECT_EQ(r.call.requests, (std::vector<std::pair<int64_t, char>>{{0, 'E'}, {500, 'D'}, {3000, 'E'}}));
  r.call.Pump();
  EXPECT_EQ(r.call.camera_rosters, 3);
}

// A real background 1 s after opening: the pause goes at once.
TEST(CallLocalHoldTest, ARealBackgroundRightAfterOpeningPausesAtOnce) {
  Rig r;
  r.CameraOn(0);
  r.Bg(1000);
  ASSERT_EQ(r.call.Count('P'), 1);
  EXPECT_EQ(r.call.requests.back().first, 1000);
}

// Ten quick taps in the foreground: every close at once, every open >= 2.5 s after the last close, one roster each.
TEST(CallLocalHoldTest, TenQuickTapsKeepTheGateAndTheRosterCount) {
  Rig r;
  for (int i = 0; i <= 10; ++i) {
    const int64_t t = i * 300;
    for (int64_t k = (i == 0 ? 0 : (i - 1) * 300 + 100); k < t; k += 100) {
      r.Tick(k);
    }
    r.Tap(t);
    r.call.Pump();
  }
  r.Run(3100, 12000);
  int64_t last_close = INT64_MIN / 4;
  for (const auto& [t, kind] : r.call.requests) {
    if (kind == 'E') {
      EXPECT_GE(t - last_close, CallLocalHold::kCameraSettleMs) << "an open at " << t;
    } else {
      last_close = t;
    }
  }
  EXPECT_EQ(static_cast<size_t>(r.call.camera_rosters), r.call.requests.size()) << "one roster per request";
  EXPECT_TRUE(r.hold.WantsCameraOn()) << "the eleventh tap is an on";
}

// The wall clock plays no part: it jumping back an hour does not hurt the gate, and its jump forward does not open it.
TEST(CallLocalHoldTest, TheGateUsesTheMonotonicClock) {
  Rig r;
  r.CameraOn(0);
  r.Tap(100); // our close, mono 100
  r.Tap(200);
  CallLocalHold::Facts f = In("c1", false, 1'000'000'000);
  f.mono_ms = 2400; // not yet
  r.call.now = 2400;
  r.hold.Sync(f);
  EXPECT_EQ(r.call.Count('E'), 1) << "the wall clock far ahead opens nothing";
  f.now_ms = 1'000'000'000 - 3'600'000; // the wall clock jumped back an hour
  f.mono_ms = 2600;
  r.call.now = 2600;
  r.hold.Sync(f);
  EXPECT_EQ(r.call.Count('E'), 2) << "the monotonic 2.5 s passed";
}

// A camera paused for the background and then closed by the user: the close clears "paused" and nothing reopens it.
TEST(CallLocalHoldTest, ACloseWhilePausedClearsPausedAndStaysClosed) {
  Rig r;
  r.CameraOn(0);
  r.Bg(1000);
  r.call.Pump();
  ASSERT_TRUE(r.call.roster_video_paused);
  r.WillFg();
  r.Active(2000);
  r.Tap(2200); // back and counting the settle (the signal was not lost): the button says on, the tap is "off"
  EXPECT_EQ(r.call.Count('D'), 1);
  r.call.Pump();
  EXPECT_FALSE(r.call.roster_video_paused) << "the close carries video_paused = false";
  r.Run(2300, 20000);
  EXPECT_EQ(r.call.Count('E'), 1) << "never reopened";
  EXPECT_FALSE(r.call.engine_camera);
}

// The engine failed the camera (denied, open failed, gave up): the wish goes with it, and the retry waits like after any
// close; a camera narrowed to voice is our close, so the gate counts too.
TEST(CallLocalHoldTest, AnEngineFailureTakesTheWishAndDoesNotGateTheRetry) {
  Rig r;
  r.CameraOn(0);
  r.call.engine_camera = false;
  r.hold.OnCameraClosedByEngine(500);
  EXPECT_FALSE(r.hold.WantsCameraOn());
  r.Tap(1000);
  EXPECT_EQ(r.call.Count('E'), 1) << "the engine's close counts for the gate: held";
  EXPECT_TRUE(r.hold.CameraResuming());
  r.Run(1100, 2900);
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(3000);
  EXPECT_EQ(r.call.Count('E'), 2) << "reopened 2.5 s after the engine's close";
  EXPECT_EQ(r.call.requests.back().first, 3000);
  r.call.Pump();

  Rig n;
  n.CameraOn(0);
  n.call.now = 5000;
  n.hold.OnCameraClosedByUs(5000);
  EXPECT_FALSE(n.hold.WantsCameraOn());
  n.Tap(7400);
  EXPECT_EQ(n.call.Count('E'), 1) << "2.4 s after our close: held";
  n.Tick(7500);
  EXPECT_EQ(n.call.Count('E'), 2);
}

// An open the call refuses: the wish drops; a tap's refusal is said to the user, the automatic one is not.
TEST(CallLocalHoldTest, ARefusedOpenDropsTheWish) {
  Rig r;
  r.call.refuse_enable = true;
  r.Tap(0);
  r.call.Pump();
  EXPECT_FALSE(r.hold.WantsCameraOn());
  EXPECT_EQ(r.call.open_failed, 1);

  Rig a;
  a.call.refuse_enable = true;
  a.Auto(0);
  a.call.Pump();
  EXPECT_FALSE(a.hold.WantsCameraOn());
  EXPECT_EQ(a.call.open_failed, 0);
}

// The camera permission prompt makes the app inactive while the engine shows "open, no frames yet": nothing is read
// back, nothing is asked twice.
TEST(CallLocalHoldTest, ThePermissionPromptReadsNothingBack) {
  Rig r;
  r.CameraOn(0);
  r.Inactive(100);
  r.Run(200, 4000);
  r.Active(4100); // the user allowed
  r.Run(4200, 8000);
  EXPECT_TRUE(r.hold.WantsCameraOn());
  EXPECT_EQ(r.call.requests.size(), 1u);
  EXPECT_FALSE(r.hold.CameraResuming());
}

// A hold that ends while the app is not yet settled opens only once it is; a hold that rested the camera 1 s ago waits
// for the gate.
TEST(CallLocalHoldTest, AHoldsEndWaitsForTheSettle) {
  Rig r;
  r.CameraOn(0);
  r.audio = true;
  r.Tick(1000); // rested
  r.Bg(1200);
  r.WillFg();
  r.Active(1300); // settled at 3800
  r.audio = false;
  r.Tick(1500); // the hold ends
  r.Run(1600, 3700);
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(3800);
  EXPECT_EQ(r.call.Count('E'), 2);
}

// ---- review fixes (2026-11-15) ----------------------------------------------------------------------------------------

// Cold start in the background (a VoIP push while locked): the app never saw a background, yet it is not shown. The
// automatic camera-on opens nothing then; the first "active" plus 2.5 s opens exactly once, with no WILL_ENTER_FOREGROUND.
TEST(CallLocalHoldTest, AColdStartInTheBackgroundOpensNothingUntilActiveAndSettled) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", false, 0, /*foreground=*/true, /*visible=*/false));
  call.now = 100;
  hold.OnAutoCameraOn(100);
  EXPECT_TRUE(hold.WantsCameraOn());
  for (int64_t t = 200; t <= 30000; t += 100) {
    call.now = t;
    call.Pump();
    hold.Sync(In("c1", false, t, true, false));
  }
  EXPECT_TRUE(call.requests.empty()) << "zero open requests in the background";
  hold.OnActive(31000);
  for (int64_t t = 31100; t < 33500; t += 100) {
    call.now = t;
    hold.Sync(In("c1", false, t));
  }
  EXPECT_TRUE(call.requests.empty()) << "2.4 s after the first active";
  call.now = 33500;
  hold.Sync(In("c1", false, 33500));
  EXPECT_EQ(call.Count('E'), 1) << "exactly one open, 2.5 s after active";
  hold.OnActive(33600); // a repeated active changes nothing
  hold.Sync(In("c1", false, 40000));
  EXPECT_EQ(call.requests.size(), 1u);
}

// Never settled (the active edge was lost) and the user taps: the tap is "resume", for a wish that is on and for one
// that is off.
TEST(CallLocalHoldTest, ATapOnANeverSettledColdStartResumes) {
  FakeCall on;
  CallLocalHold hold_on(on.Ports());
  hold_on.Sync(In("c1", false, 0, true, false));
  hold_on.OnAutoCameraOn(100);
  hold_on.OnCameraTapped(5000); // the button says on; the app is plainly active: resume, not off
  EXPECT_TRUE(hold_on.WantsCameraOn());
  EXPECT_EQ(on.Count('D'), 0);
  hold_on.Sync(In("c1", false, 7499));
  EXPECT_TRUE(on.requests.empty());
  hold_on.Sync(In("c1", false, 7500));
  EXPECT_EQ(on.Count('E'), 1);

  FakeCall off;
  CallLocalHold hold_off(off.Ports());
  hold_off.Sync(In("c1", false, 0, true, false));
  hold_off.OnCameraTapped(5000);
  EXPECT_TRUE(hold_off.WantsCameraOn());
  hold_off.Sync(In("c1", false, 7499));
  EXPECT_TRUE(off.requests.empty());
  hold_off.Sync(In("c1", false, 7500));
  EXPECT_EQ(off.Count('E'), 1);
}

// The call id flickers (c1 -> "" -> c1) while the device keeps streaming: the new call adopts the camera that is on.
TEST(CallLocalHoldTest, ACallIdFlickerAdoptsTheCameraThatIsOn) {
  Rig r;
  r.CameraOn(0);
  const size_t requests = r.call.requests.size();
  r.hold.Sync(In("", false, 1000));
  r.hold.Sync(In("c1", false, 1100));
  EXPECT_TRUE(r.hold.WantsCameraOn()) << "the button says on: the camera is sending";
  EXPECT_EQ(r.call.requests.size(), requests) << "no request";
  r.Run(1200, 9000);
  EXPECT_EQ(r.call.requests.size(), requests);
  EXPECT_TRUE(r.call.engine_camera);
  r.Tap(9100); // a later tap off really closes it
  EXPECT_FALSE(r.hold.WantsCameraOn());
  EXPECT_EQ(r.call.Count('D'), 1);
  r.call.Pump();
  EXPECT_FALSE(r.call.engine_camera);

  FakeCall call; // from the start: Sync("") then Sync("c1") with the engine camera on
  CallLocalHold hold(call.Ports());
  hold.Sync(In("", false, 0));
  call.engine_camera = true;
  hold.Sync(In("c1", false, 10));
  EXPECT_TRUE(hold.WantsCameraOn());
  EXPECT_TRUE(call.requests.empty());
  hold.OnCameraTapped(5000);
  EXPECT_EQ(call.Count('D'), 1);
}

// No open request while the app is inactive (iOS): the gate expiring in the middle of the lock screen's flicker, then
// the background callback, opens nothing at all.
TEST(CallLocalHoldTest, NoOpenIsSentWhileInactive) {
  Rig r;
  r.CameraOn(0);
  r.Tap(100); // off: our close at 100, the gate ends at 2600
  r.Tick(200);
  r.Tap(2000); // on again, waiting for the gate
  r.Inactive(2400);
  r.Active(2450); // the lone active the lock screen sends
  r.Inactive(2460);
  r.Tick(2600); // the gate has expired, the app is inactive
  r.Tick(2700);
  EXPECT_EQ(r.call.Count('E'), 1) << "no open while inactive";
  r.Bg(2710); // the background callback
  r.call.Pump();
  r.Run(2800, 30000);
  EXPECT_EQ(r.call.Count('E'), 1) << "zero opens, ever";
  EXPECT_FALSE(r.call.engine_camera);

  // Android may deliver will-enter-background without a later did-enter-foreground: its opens do not wait for active.
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", false, 0));
  hold.OnCameraTapped(0);
  hold.OnCameraTapped(100);
  hold.OnInactive();
  CallLocalHold::Facts f = In("c1", false, 200);
  f.open_needs_active = false;
  hold.OnCameraTapped(200);
  hold.Sync(f);
  f.now_ms = f.mono_ms = 2700;
  hold.Sync(f);
  EXPECT_EQ(call.Count('E'), 2) << "Android: open without 'active'";
}

// The inactive wait is not an inactive pause: a camera that is on stays on, and the open goes out after active.
TEST(CallLocalHoldTest, AnOpenWaitingForActiveGoesOutAtActive) {
  Rig r;
  r.Inactive(100);
  r.Tap(200);
  EXPECT_TRUE(r.hold.WantsCameraOn());
  r.Run(300, 5000);
  EXPECT_EQ(r.call.requests.size(), 0u);
  r.Active(5100);
  EXPECT_EQ(r.call.Count('E'), 1);
}

// The real B225 order: the lone active BEFORE the true background callback. The camera is paused once and never opens.
TEST(CallLocalHoldTest, TheLoneActiveBeforeTheTrueBackgroundNeverOpens) {
  Rig r;
  r.CameraOn(0);
  const int opens = r.call.device_opens;
  r.Inactive(1000);
  r.Active(1120);
  r.Inactive(1128);
  r.call.Pump();
  r.Bg(1135);
  r.call.Pump();
  r.Run(1200, 61000);
  EXPECT_EQ(r.call.Count('P'), 1);
  EXPECT_EQ(r.call.Count('E'), 1);
  EXPECT_EQ(r.call.device_opens, opens);
  EXPECT_FALSE(r.call.engine_camera);
}

// Duplicate events change nothing: two backgrounds, two will-enter-foregrounds, two actives.
TEST(CallLocalHoldTest, DuplicateLifecycleEventsAreIdempotent) {
  Rig r;
  r.CameraOn(0);
  r.Bg(1000);
  r.Bg(1000);
  r.call.Pump();
  EXPECT_EQ(r.call.Count('P'), 1);
  r.WillFg();
  r.WillFg();
  r.Active(2000);
  r.Active(2100); // does not push the settle later
  r.Active(2200);
  r.Run(2300, 4400);
  EXPECT_EQ(r.call.Count('E'), 1);
  r.Tick(4500);
  EXPECT_EQ(r.call.Count('E'), 2) << "2.5 s after the first active";
  r.Bg(5000);
  r.Bg(5000);
  r.call.Pump();
  r.Run(5100, 20000);
  EXPECT_EQ(r.call.Count('E'), 2);
  EXPECT_EQ(r.call.Count('P'), 2);
}

// A failure hint is for a request the user tapped: a later automatic reopen (after the background) that the call
// refuses says nothing.
TEST(CallLocalHoldTest, ARefusedReopenAfterTheBackgroundSaysNothing) {
  Rig r;
  r.Tap(0); // the tap's open goes out and lands
  r.call.Pump();
  EXPECT_EQ(r.call.open_failed, 0);
  r.Bg(1000);
  r.call.Pump();
  r.WillFg();
  r.Active(2000);
  r.call.refuse_enable = true;
  r.Run(2100, 4600);
  r.call.Pump();
  EXPECT_FALSE(r.hold.WantsCameraOn());
  EXPECT_EQ(r.call.open_failed, 0) << "the reopen was not asked for by a tap";

  Rig t; // a tap that waited for the gate and is then refused is still said
  t.Auto(0);
  t.call.Pump();
  t.Tap(100); // off
  t.Tap(200); // on, gated
  t.call.refuse_enable = true;
  t.Run(300, 3000);
  t.call.Pump();
  EXPECT_EQ(t.call.open_failed, 1);
}

} // namespace
} // namespace pbr
