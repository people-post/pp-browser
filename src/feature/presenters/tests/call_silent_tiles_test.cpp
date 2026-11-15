// DEV/PLAN-call-silent-tile.md: a group call's participant that sends nothing for 15 s is dimmed (the avatar, never a
// cached picture, and "connection lost"), restored by the next packet. The fake clock is the caller's monotonic one:
// the class has no wall clock to jump.
#include "feature/presenters/CallSilentTiles.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

using Person = CallSilentTiles::Person;

struct Rig {
  std::vector<std::string> lines;
  CallSilentTiles tiles;
  int64_t now = 1'000'000;
  bool online = true;
  std::vector<Person> people;

  explicit Rig(CallSilentTiles::Options options = {})
      : tiles([this](const std::string& line) { lines.push_back(line); }, options) {}

  Person& Add(const std::string& id) { return people.emplace_back(Person{.identity = id}); }
  Person& P(const std::string& id) {
    for (Person& p : people) {
      if (p.identity == id) {
        return p;
      }
    }
    return Add(id);
  }
  void Tick(const int64_t ms = 500) {
    now += ms;
    tiles.Step(now, online, people);
  }
  /** Ticks every 500 ms for `seconds`. */
  void Run(const int64_t seconds) {
    for (int64_t t = 0; t < seconds * 1000; t += 500) {
      Tick();
    }
  }
  bool Silent(const std::string& id) const { return tiles.TileOf(id).silent; }
  bool Blocked(const std::string& id) const { return tiles.TileOf(id).video_blocked; }
  /** One packet from `id` (audio), on the next tick. */
  void Audio(const std::string& id) {
    ++P(id).audio;
    Tick();
  }
  void Video(const std::string& id) {
    ++P(id).video;
    Tick();
  }
};

/** A rig where `id` has been received from once, at the start. */
Rig Heard(const std::string& id = "alice") {
  Rig r;
  r.Add(id);
  r.Tick();        // the join reading: references only
  r.Audio(id);     // a first packet
  return r;
}

TEST(CallSilentTilesTest, DimsAfterFifteenSecondsNotBefore) {
  Rig r = Heard();
  r.Run(14);
  r.Tick(400); // 14.4 s after the last packet... plus the tick of the packet: still short of 15 s
  EXPECT_FALSE(r.Silent("alice"));
  r.Run(1);
  EXPECT_TRUE(r.Silent("alice"));
  EXPECT_TRUE(r.Blocked("alice"));
}

TEST(CallSilentTilesTest, TheBoundaryIsExactlyFifteenSeconds) {
  Rig r = Heard();
  r.Run(14);
  r.Tick(900); // 14.9 s since the packet
  EXPECT_FALSE(r.Silent("alice"));
  r.Tick(100);
  EXPECT_TRUE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, SomeoneNeverReceivedFromIsNeverDimmed) {
  Rig r;
  r.Add("alice");
  r.Tick();
  r.Run(15);
  EXPECT_FALSE(r.Silent("alice"));
  r.Run(45); // 60 s
  EXPECT_FALSE(r.Silent("alice"));
  EXPECT_FALSE(r.Blocked("alice")) << "the tile is what it is today";
  r.Audio("alice"); // the first packet puts them in the rule
  r.Run(14);
  EXPECT_FALSE(r.Silent("alice"));
  r.Run(2);
  EXPECT_TRUE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, AnyPacketRestoresAndAudioAloneKeepsThePictureBlockedUntilVideoIncreases) {
  Rig r = Heard();
  r.P("alice").video = 5;
  r.Tick(); // the video counter first seen at 5 is only a reference...
  // ...which is a packet: it did increase from 0.
  r.Run(16);
  ASSERT_TRUE(r.Silent("alice"));
  r.Audio("alice");
  EXPECT_FALSE(r.Silent("alice")) << "an audio packet restores the tile";
  EXPECT_TRUE(r.Blocked("alice")) << "but no old picture comes back: no video packet since";
  r.Audio("alice");
  EXPECT_TRUE(r.Blocked("alice"));
  r.Video("alice");
  EXPECT_FALSE(r.Blocked("alice")) << "the picture may come back once video arrives";
}

TEST(CallSilentTilesTest, AudioAndVideoAreCountedApartAndOnlyAnIncreaseIsAPacket) {
  Rig r = Heard();
  r.P("alice").video = 10;
  r.Tick(); // video went 0 -> 10: an increase
  r.Run(10);
  // Audio is rebuilt (reads 0) while video rises: video still counts.
  r.P("alice").audio = 0;
  ++r.P("alice").video;
  r.Tick();
  r.Run(14);
  EXPECT_FALSE(r.Silent("alice")) << "one counter rising while the other fell is a packet";
  // Neither rises: audio reset to a lower value again, video flat.
  r.P("alice").audio = 0;
  r.Run(2);
  EXPECT_TRUE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, AResetOrDecreaseOnlyRecordsTheReference) {
  Rig r = Heard();
  r.P("alice").audio = 50;
  r.Tick(); // up to 50
  r.Run(10);
  r.P("alice").audio = 0; // the engine cleared its tracks: not a packet, the silence goes on from 10 s ago
  r.Tick();
  r.Run(4);
  EXPECT_FALSE(r.Silent("alice"));
  r.Run(1);
  r.Tick();
  EXPECT_TRUE(r.Silent("alice")) << "the timer was not refreshed by the reset";

  // A dimmed tile is not restored by a reset, only by an increase from the new reference.
  r.P("alice").audio = 0;
  r.Run(2);
  EXPECT_TRUE(r.Silent("alice"));
  r.P("alice").audio = 7;
  r.Run(1);
  r.P("alice").audio = 3; // fell again
  r.Tick();
  EXPECT_FALSE(r.Silent("alice")) << "0 -> 7 was an increase";
  r.P("alice").audio = 2;
  r.Run(16);
  EXPECT_TRUE(r.Silent("alice"));
  r.P("alice").audio = 1;
  r.Tick();
  EXPECT_TRUE(r.Silent("alice")) << "a decrease does not restore";
  r.P("alice").audio = 2;
  r.Tick();
  EXPECT_FALSE(r.Silent("alice")) << "from the new reference an increase does";
}

TEST(CallSilentTilesTest, MutedWithCameraOffStillSendsSilenceFramesSoIsNotDimmed) {
  Rig r = Heard();
  for (int i = 0; i < 120; ++i) { // 60 s of audio packets every second
    r.Run(1);
    ++r.P("alice").audio;
  }
  EXPECT_FALSE(r.Silent("alice"));
  EXPECT_FALSE(r.Blocked("alice"));
}

TEST(CallSilentTilesTest, OnlyVideoRisingIsNotDimmedEither) {
  Rig r = Heard();
  for (int i = 0; i < 40; ++i) {
    r.Run(1);
    ++r.P("alice").video;
  }
  EXPECT_FALSE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, OwnPathOfflineDimsNobodyNewAndKeepsTheDimmedOnesDimmed) {
  Rig r;
  r.Add("alice");
  r.Add("bob");
  r.Tick();
  r.Audio("alice");
  r.Audio("bob");
  r.Run(16);
  ASSERT_TRUE(r.Silent("alice"));
  r.Audio("bob");
  r.Run(5);
  // bob has been quiet 5 s; now our path drops for a minute.
  r.online = false;
  r.Run(60);
  EXPECT_FALSE(r.Silent("bob")) << "nobody new while we are offline";
  EXPECT_TRUE(r.Silent("alice")) << "the dimmed one stays";
  EXPECT_TRUE(r.Blocked("alice"));
  // It comes back: the dimmed one stays, nobody new for 14 s.
  r.online = true;
  r.Tick();
  EXPECT_TRUE(r.Silent("alice")) << "no flash back to lit";
  EXPECT_TRUE(r.Blocked("alice")) << "the cached picture does not return";
  r.Run(13);
  EXPECT_FALSE(r.Silent("bob"));
  r.Run(2);
  EXPECT_TRUE(r.Silent("bob")) << "15 s with no packet after the path came back";
  // And alice returns only with a real packet.
  r.Audio("alice");
  EXPECT_FALSE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, PacketsWhileOwnPathIsOfflineStillRestore) {
  Rig r = Heard();
  r.Run(16);
  ASSERT_TRUE(r.Silent("alice"));
  r.online = false;
  r.Run(2);
  EXPECT_TRUE(r.Silent("alice"));
  r.Audio("alice");
  EXPECT_FALSE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, ATickGapOverThreeSecondsRestartsTheBaselineAndThreeDoesNot) {
  Rig r = Heard();
  r.Run(10);
  r.Tick(3'000); // exactly 3 s: no reset
  r.Tick(2'100); // the silence is now 15.1 s from the packet
  EXPECT_TRUE(r.Silent("alice"));

  Rig g = Heard();
  g.Run(10);
  g.Tick(40'000); // the UI was suspended for 40 s
  EXPECT_FALSE(g.Silent("alice")) << "nobody is dimmed in bulk after the gap";
  g.Run(14);
  EXPECT_FALSE(g.Silent("alice"));
  g.Run(2);
  EXPECT_TRUE(g.Silent("alice")) << "15 s after the gap with no packet";
}

TEST(CallSilentTilesTest, JustJoinedAndRejoinedGetAFreshFifteenSeconds) {
  Rig r = Heard();
  r.Run(30);
  ASSERT_TRUE(r.Silent("alice"));
  r.people.clear(); // they leave: their state is dropped
  r.Tick();
  EXPECT_FALSE(r.Silent("alice"));
  EXPECT_FALSE(r.Blocked("alice"));
  r.Add("alice"); // and come back on the same stream
  r.Tick();
  r.Audio("alice");
  r.Run(14);
  EXPECT_FALSE(r.Silent("alice"));
  r.Run(2);
  EXPECT_TRUE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, ARejoinerWithNoPacketsIsBackToNeverReceived) {
  Rig r = Heard();
  r.people.clear();
  r.Tick();
  r.Add("alice");
  r.Tick();
  r.Run(60);
  EXPECT_FALSE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, TwoPeopleOnTheSameCountersCoverForEachOther) {
  // Their stream ids collided: both read the same counters, so one being alive keeps the other lit.
  Rig r;
  r.Add("alice");
  r.Add("alias");
  r.Tick();
  for (int i = 0; i < 40; ++i) {
    r.Run(1);
    ++r.P("alice").audio;
    r.P("alias").audio = r.P("alice").audio;
  }
  EXPECT_FALSE(r.Silent("alice"));
  EXPECT_FALSE(r.Silent("alias"));
}

TEST(CallSilentTilesTest, OnHoldIsNotDimmedAndOnlyOnePillShows) {
  Rig r = Heard();
  r.P("alice").on_hold = true;
  r.Run(30);
  EXPECT_FALSE(r.Silent("alice")) << "the on-hold pill is the one";
  r.P("alice").on_hold = false;
  r.Tick();
  EXPECT_TRUE(r.Silent("alice")) << "back from hold with no packet: dimmed";
  r.P("alice").on_hold = true;
  r.Tick();
  EXPECT_FALSE(r.Silent("alice"));
  EXPECT_TRUE(r.Blocked("alice")) << "a dimmed one on hold still shows no old picture";
  r.P("alice").on_hold = false;
  r.Audio("alice");
  EXPECT_FALSE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, AnotherPersonAliveDoesNotSaveTheSilentOne) {
  Rig r;
  r.Add("alice");
  r.Add("bob");
  r.Tick();
  r.Audio("alice");
  r.Audio("bob");
  for (int i = 0; i < 20; ++i) {
    r.Run(1);
    ++r.P("bob").audio;
  }
  EXPECT_TRUE(r.Silent("alice"));
  EXPECT_FALSE(r.Silent("bob"));
}

TEST(CallSilentTilesTest, ResetForgetsTheCall) {
  Rig r = Heard();
  r.Run(16);
  ASSERT_TRUE(r.Silent("alice"));
  r.tiles.Reset();
  EXPECT_FALSE(r.Silent("alice"));
}

TEST(CallSilentTilesTest, OneLogLinePerTransitionWithBucketAndBaselineAndNothingElse) {
  Rig r = Heard("alice@acct-secret-id");
  r.Run(16);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_EQ(r.lines[0], "call tile dimmed silent=15-20s baseline=packet");
  r.Audio("alice@acct-secret-id");
  ASSERT_EQ(r.lines.size(), 2u);
  EXPECT_EQ(r.lines[1].rfind("call tile restored silent=", 0), 0u);
  for (const std::string& line : r.lines) {
    EXPECT_EQ(line.find("alice"), std::string::npos);
    EXPECT_EQ(line.find("acct"), std::string::npos);
  }
  r.Run(5);
  EXPECT_EQ(r.lines.size(), 2u) << "no line while nothing changes";
}

TEST(CallSilentTilesTest, TheLogSaysWhichBaselineApplied) {
  // Own baseline: the path came back, and 15 s later nothing arrived.
  Rig r = Heard();
  r.online = false;
  r.Run(20);
  r.online = true;
  r.Run(16);
  ASSERT_FALSE(r.lines.empty());
  EXPECT_EQ(r.lines.back(), "call tile dimmed silent=15-20s baseline=own");

  // Join baseline: a rejoiner whose only packet came before the join reading cannot be older than the join.
  Rig j;
  j.Add("bob");
  j.Tick();
  j.Audio("bob");
  j.Run(70);
  ASSERT_EQ(j.lines.size(), 1u);
  EXPECT_NE(j.lines[0].find("silent=15-20s"), std::string::npos);
  // (the packet is later than the join, so it is the packet that applies)
  EXPECT_NE(j.lines[0].find("baseline=packet"), std::string::npos);
}

TEST(CallSilentTilesTest, TheBucketsOfTheSilenceAtDimmingAndAtRestoring) {
  // Dimmed on a coarse tick: 17 s.
  Rig r = Heard();
  r.Run(14);
  r.Tick(3'000);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_EQ(r.lines[0], "call tile dimmed silent=15-20s baseline=packet") << "a 3 s step is no gap";

  Rig m = Heard();
  m.Run(30);
  m.Audio("alice");
  ASSERT_EQ(m.lines.size(), 2u);
  EXPECT_EQ(m.lines[1], "call tile restored silent=20-60s baseline=packet");

  Rig l = Heard();
  l.Run(90);
  l.Audio("alice");
  ASSERT_EQ(l.lines.size(), 2u);
  EXPECT_EQ(l.lines[1], "call tile restored silent=60s+ baseline=packet");
}

TEST(CallSilentTilesTest, FallbackOffByDefaultTwoQuietTogetherBothDim) {
  Rig r;
  r.Add("alice");
  r.Add("bob");
  r.Tick();
  r.Audio("alice");
  r.Audio("bob");
  r.Run(16);
  EXPECT_TRUE(r.Silent("alice"));
  EXPECT_TRUE(r.Silent("bob"));
}

TEST(CallSilentTilesTest, FallbackEveryoneStoppingTogetherIsOurOwnPathSoNobodyDims) {
  Rig r({.all_stop_fallback = true});
  r.Add("alice");
  r.Add("bob");
  r.Tick();
  ++r.P("alice").audio;
  ++r.P("bob").audio;
  r.Tick(); // both heard on the same tick, then both stop at once
  r.Run(40);
  EXPECT_FALSE(r.Silent("alice"));
  EXPECT_FALSE(r.Silent("bob"));
  // One of them is heard again: the other one, still silent, dims from the next tick.
  r.Audio("alice");
  EXPECT_FALSE(r.Silent("alice"));
  EXPECT_TRUE(r.Silent("bob"));
}

TEST(CallSilentTilesTest, FallbackOneStopsTheOthersGoOnThenTheStoppedOneDims) {
  Rig r({.all_stop_fallback = true});
  r.Add("alice");
  r.Add("bob");
  r.Tick();
  r.Audio("alice");
  r.Audio("bob");
  for (int i = 0; i < 20; ++i) {
    r.Run(1);
    ++r.P("bob").audio;
  }
  EXPECT_TRUE(r.Silent("alice"));
  EXPECT_FALSE(r.Silent("bob"));
}

TEST(CallSilentTilesTest, FallbackNeedsTwoPeopleToCompare) {
  Rig r({.all_stop_fallback = true});
  r.Add("alice");
  r.Tick();
  r.Audio("alice");
  r.Run(16);
  EXPECT_TRUE(r.Silent("alice")) << "with one other person only the own-path signals can protect";
}

} // namespace
} // namespace pbr
