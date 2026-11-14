#include "feature/presenters/CallLocalHold.h"

#include <gtest/gtest.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

/** A call's media as the hold sees it: the mic, the camera, and the call's answers to "tell them we are on hold". */
struct FakeCall {
  bool muted = false;
  bool camera = false;
  /** Publishes are answered when the test says (SetLocalOnHold replies later, on UI). */
  std::vector<std::pair<bool, std::function<void(Roe<void>)>>> publishes;
  std::vector<bool> told;
  /** B221: every roster that would leave, in order: what the others see of {muted, on_hold} after it. */
  std::vector<std::pair<bool, bool>> rosters; // {muted, on_hold}
  bool on_hold_row = false;
  std::vector<bool> camera_pauses;
  std::vector<std::string> notes;

  CallLocalHold::Ports Ports() {
    CallLocalHold::Ports ports;
    ports.mic_muted = [this]() { return muted; };
    ports.camera_on = [this]() { return camera; };
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
    ports.set_camera_paused = [this](bool paused) { camera_pauses.push_back(paused); };
    ports.note = [this](const std::string& key) { notes.push_back(key); };
    return ports;
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

CallLocalHold::Facts In(const std::string& call_id, bool audio_taken, int64_t now_ms, bool foreground = true) {
  return {.call_id = call_id, .audio_taken = audio_taken, .foreground = foreground, .now_ms = now_ms};
}

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

// The camera rests during the hold and comes back at its end — in the background only once the app is in front.
TEST(CallLocalHoldTest, TheCameraRestsDuringTheHold) {
  FakeCall call;
  call.camera = true;
  CallLocalHold hold(call.Ports());
  hold.Sync(In("c1", true, 0));
  EXPECT_TRUE(hold.CameraPaused());
  hold.OnAppForeground();
  EXPECT_TRUE(hold.CameraPaused()) << "on hold: the foreground does not bring it back";
  hold.Sync(In("c1", false, 10, /*foreground=*/false));
  EXPECT_TRUE(hold.CameraPaused()) << "still in the background";
  hold.OnAppForeground();
  EXPECT_FALSE(hold.CameraPaused());
  EXPECT_EQ(call.camera_pauses, (std::vector<bool>{true, false}));
}

// A camera tap during the hold takes the rest off (the camera still counts as on) and nothing else.
TEST(CallLocalHoldTest, ACameraTapDuringTheHoldBringsTheCameraBack) {
  FakeCall call;
  call.camera = true;
  CallLocalHold hold(call.Ports());
  EXPECT_FALSE(hold.TakeCameraTap()) << "no hold: the button toggles as usual";
  hold.Sync(In("c1", true, 0));
  EXPECT_TRUE(hold.TakeCameraTap());
  EXPECT_FALSE(hold.CameraPaused());
  EXPECT_FALSE(hold.TakeCameraTap()) << "the next tap is the button's";
  hold.Sync(In("c1", false, 10));
  EXPECT_EQ(call.camera_pauses, (std::vector<bool>{true, false})) << "the hold's end has nothing to undo";
}

// Mobile: the background rests a camera that is on; the foreground brings it back.
TEST(CallLocalHoldTest, TheBackgroundRestsTheCamera) {
  FakeCall call;
  CallLocalHold hold(call.Ports());
  hold.OnAppBackground();
  EXPECT_FALSE(hold.CameraPaused()) << "camera off: nothing to rest";
  call.camera = true;
  hold.OnAppBackground();
  EXPECT_TRUE(hold.CameraPaused());
  hold.OnAppForeground();
  EXPECT_FALSE(hold.CameraPaused());
}

} // namespace
} // namespace pbr
