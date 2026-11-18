#include "lib/calls/CallParticipantLiveness.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

// V067: each receiver watches every participant of a group call by their 40 ms frames.

using Strings = std::vector<std::string>;

/** One call with Bob and Carol in it; `frames` says when each was last heard (0: never). */
struct Call {
  ParticipantLivenessWatch watch;
  int64_t bob_ms = 0;
  int64_t carol_ms = 0;
  bool live = true;

  ParticipantLivenessStep At(const int64_t now) {
    ParticipantLivenessSample sample;
    sample.now_ms = now;
    sample.our_media_live = live;
    sample.peers.push_back({"account:bob", bob_ms});
    sample.peers.push_back({"account:carol", carol_ms});
    return watch.Step(sample);
  }

  /** Steps once a second from `from` to `to` (inclusive), Carol heard all along; the last step. */
  ParticipantLivenessStep Run(int64_t from, const int64_t to) {
    ParticipantLivenessStep step;
    for (; from <= to; from += kParticipantLivenessStepMs) {
      carol_ms = from;
      step = At(from);
      if (!step.gone.empty()) {
        return step;
      }
    }
    return step;
  }
};

TEST(ParticipantLivenessTest, SilentFramesKeepEveryoneLive) {
  Call call;
  for (int64_t now = 1'000; now <= 120'000; now += 1'000) {
    call.bob_ms = now - 40;
    call.carol_ms = now - 40;
    const ParticipantLivenessStep step = call.At(now);
    EXPECT_TRUE(step.quiet.empty()) << now;
    EXPECT_TRUE(step.gone.empty()) << now;
  }
}

TEST(ParticipantLivenessTest, ThreeSecondsWithNothingReadsReconnecting) {
  Call call;
  call.bob_ms = 10'000;
  EXPECT_TRUE(call.Run(10'000, 12'000).quiet.empty()) << "2 s: still live";
  const ParticipantLivenessStep step = call.Run(13'000, 13'000);
  EXPECT_EQ(step.quiet, Strings{"account:bob"});
  EXPECT_TRUE(step.quiet_changed);
  EXPECT_TRUE(step.gone.empty());
  EXPECT_FALSE(call.Run(14'000, 14'000).quiet_changed) << "said once";
}

TEST(ParticipantLivenessTest, ThirtySecondsWithNothingIsGone) {
  Call call;
  call.bob_ms = 10'000;
  EXPECT_TRUE(call.Run(10'000, 39'000).gone.empty());
  const ParticipantLivenessStep step = call.Run(40'000, 40'000);
  EXPECT_EQ(step.gone, Strings{"account:bob"});
  EXPECT_EQ(step.quiet, Strings{"account:bob"}) << "reconnecting on screen until their leave lands";
  EXPECT_FALSE(step.quiet_changed);
  const ParticipantLivenessStep next = call.Run(41'000, 41'000);
  EXPECT_TRUE(next.gone.empty()) << "said once";
  EXPECT_EQ(next.quiet, Strings{"account:bob"});
  call.watch.Forget("account:bob"); // the leave landed
  EXPECT_TRUE(call.watch.Quiet().empty());
}

TEST(ParticipantLivenessTest, FramesResumingBeforeThirtySecondsClearIt) {
  Call call;
  call.bob_ms = 10'000;
  EXPECT_EQ(call.Run(10'000, 35'000).quiet, Strings{"account:bob"});
  call.bob_ms = 35'500;
  const ParticipantLivenessStep back = call.Run(36'000, 36'000);
  EXPECT_TRUE(back.quiet.empty());
  EXPECT_TRUE(back.quiet_changed);
  EXPECT_TRUE(call.Run(37'000, 64'000).gone.empty()) << "counted from the frame that came back";
  EXPECT_EQ(call.Run(65'000, 66'000).gone, Strings{"account:bob"});
}

TEST(ParticipantLivenessTest, OurOwnReconnectPausesEveryClock) {
  Call call;
  call.bob_ms = 10'000;
  EXPECT_EQ(call.Run(10'000, 20'000).quiet, Strings{"account:bob"});
  call.live = false; // our hop is being reattached: nothing reaches us from anyone
  call.carol_ms = 20'000;
  for (int64_t now = 21'000; now <= 90'000; now += 1'000) {
    const ParticipantLivenessStep step = call.At(now);
    EXPECT_TRUE(step.gone.empty()) << "our drop removes nobody";
    EXPECT_TRUE(step.quiet.empty()) << "nobody dimmed while it is us";
  }
  call.live = true;
  EXPECT_TRUE(call.Run(91'000, 93'000).quiet.empty()) << "the clocks start when we are back";
  EXPECT_EQ(call.Run(94'000, 94'000).quiet, Strings{"account:bob"});
  EXPECT_TRUE(call.Run(95'000, 120'000).gone.empty());
  EXPECT_EQ(call.Run(121'000, 121'000).gone, Strings{"account:bob"});
}

TEST(ParticipantLivenessTest, OurAppAsleepStartsTheClocksOver) {
  Call call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000;
  EXPECT_TRUE(call.At(10'000).quiet.empty());
  // Backgrounded for 20 s: no step ran, no frame was read.
  const ParticipantLivenessStep woke = call.At(30'000);
  EXPECT_TRUE(woke.gone.empty());
  EXPECT_TRUE(woke.quiet.empty());
  EXPECT_FALSE(woke.away_too_long) << "shorter than the others' wait";
  EXPECT_TRUE(call.Run(31'000, 59'000).gone.empty()) << "counted from waking";
  EXPECT_EQ(call.Run(60'000, 61'000).gone, Strings{"account:bob"});
}

TEST(ParticipantLivenessTest, AsleepForTheWholeWindowIsAwayTooLong) {
  Call call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000;
  EXPECT_TRUE(call.At(10'000).quiet.empty());
  const ParticipantLivenessStep woke = call.At(10'000 + kParticipantGoneMs);
  EXPECT_TRUE(woke.away_too_long) << "the others took us as gone meanwhile";
  EXPECT_TRUE(woke.gone.empty()) << "and we take nobody as gone for it";
}

TEST(ParticipantLivenessTest, NotLiveYetIsNeverAwayTooLong) {
  Call call;
  call.live = false;
  EXPECT_FALSE(call.At(10'000).away_too_long);
  EXPECT_FALSE(call.At(80'000).away_too_long) << "still attaching: we were in no media to be dropped from";
}

TEST(ParticipantLivenessTest, SomeoneNotHeardYetIsNotDimmedWhileJoining) {
  Call call; // Bob joined but his media is not up yet
  EXPECT_TRUE(call.Run(10'000, 40'000).quiet.empty());
  EXPECT_TRUE(call.Run(41'000, 10'000 + kParticipantNeverHeardGoneMs - 1'000).gone.empty())
      << "his own attach wait may still be running";
  EXPECT_EQ(call.Run(10'000 + kParticipantNeverHeardGoneMs, 10'000 + kParticipantNeverHeardGoneMs).gone,
            Strings{"account:bob"})
      << "never heard and never left: his app died while joining";
}

TEST(ParticipantLivenessTest, SomeoneWhoLeftIsForgottenAndCountsAfreshOnRejoin) {
  Call call;
  call.bob_ms = 10'000;
  EXPECT_EQ(call.Run(10'000, 20'000).quiet, Strings{"account:bob"});
  call.watch.Forget("account:bob");
  EXPECT_TRUE(call.watch.Quiet().empty());
  call.bob_ms = 0; // rejoined: his new stream not heard yet
  EXPECT_TRUE(call.Run(21'000, 60'000).quiet.empty()) << "joining again, not reconnecting";
}

TEST(ParticipantLivenessTest, TheLastOtherGoingQuietIsGoneToo) {
  // Two left in the call: the per-participant rule takes the last other as gone; this device then waits alone (V053).
  ParticipantLivenessWatch watch;
  ParticipantLivenessSample sample;
  sample.our_media_live = true;
  sample.peers.push_back({"account:bob", 10'000});
  ParticipantLivenessStep step;
  for (int64_t now = 10'000; now <= 10'000 + kParticipantGoneMs && step.gone.empty(); now += 1'000) {
    sample.now_ms = now;
    step = watch.Step(sample);
  }
  EXPECT_EQ(step.gone, Strings{"account:bob"});
}

TEST(ParticipantLivenessTest, SomeoneNoLongerJoinedIsReportedLeftOnce) {
  // Out of the call by any way the watch did not see (a roster from the key's coordinator, a removal): their stream
  // must go too.
  ParticipantLivenessWatch watch;
  ParticipantLivenessSample sample;
  sample.now_ms = 10'000;
  sample.our_media_live = true;
  sample.peers = {{"account:bob", 9'990}, {"account:carol", 9'990}};
  EXPECT_TRUE(watch.Step(sample).left.empty());
  sample.now_ms = 11'000;
  sample.peers = {{"account:carol", 10'990}};
  EXPECT_EQ(watch.Step(sample).left, Strings{"account:bob"});
  sample.now_ms = 12'000;
  EXPECT_TRUE(watch.Step(sample).left.empty()) << "said once";
}

TEST(ParticipantLivenessTest, AnUnreadableRosterTakesNobodyAsLeftAndPauses) {
  ParticipantLivenessWatch watch;
  ParticipantLivenessSample sample;
  sample.now_ms = 10'000;
  sample.our_media_live = true;
  sample.peers = {{"account:bob", 9'990}};
  ASSERT_TRUE(watch.Step(sample).left.empty());
  sample.peers.clear();
  sample.peers_known = false;
  for (int64_t now = 11'000; now <= 80'000; now += 1'000) {
    sample.now_ms = now;
    const ParticipantLivenessStep step = watch.Step(sample);
    EXPECT_TRUE(step.left.empty()) << now;
    EXPECT_TRUE(step.gone.empty()) << now;
  }
}

// --- V067 8: the call going quiet for this device ------------------------------------------------------

/** Bob and Carol in the call; when each was last heard, and when a frame last came that no key we hold opened. */
struct CutCall {
  ParticipantLivenessWatch watch;
  int64_t bob_ms = 0;
  int64_t carol_ms = 0;
  int64_t unopened_ms = 0;
  bool with_peers = true;
  bool live = true;
  /** What every step said, from the first one. */
  bool rejoined = false;
  int rejoins = 0;
  bool dimmed = false;
  bool disconnected = false;
  Strings gone;

  ParticipantLivenessStep At(const int64_t now) {
    ParticipantLivenessSample sample;
    sample.now_ms = now;
    sample.our_media_live = live;
    if (with_peers) {
      sample.peers.push_back({"account:bob", bob_ms});
      sample.peers.push_back({"account:carol", carol_ms});
    }
    sample.last_unopened_ms = unopened_ms;
    ParticipantLivenessStep step = watch.Step(sample);
    rejoins += step.rejoin ? 1 : 0;
    rejoined = rejoined || step.rejoin;
    dimmed = dimmed || !step.quiet.empty();
    disconnected = disconnected || step.disconnected;
    gone.insert(gone.end(), step.gone.begin(), step.gone.end());
    return step;
  }
  /** Steps once a second from `from` to `to` (inclusive); frames that do not open keep coming when `shut_out`. */
  ParticipantLivenessStep Run(int64_t from, const int64_t to, const bool shut_out = false) {
    ParticipantLivenessStep step;
    for (; from <= to; from += kParticipantLivenessStepMs) {
      if (shut_out) {
        unopened_ms = from - 40;
      }
      step = At(from);
    }
    return step;
  }
};

TEST(ParticipantCutOffTest, EveryoneStoppingAtOnceIsOurOwnNetworkNotTheirs) {
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'200; // their last frames within a moment of each other: then nothing from anyone
  const ParticipantLivenessStep quiet = call.Run(10'000, 14'000);
  EXPECT_TRUE(quiet.cut_off) << "the call reads Reconnecting for us";
  EXPECT_FALSE(call.dimmed) << "nobody is dimmed: it is not one of them (not Bob alone either, a moment before)";
  call.Run(15'000, 10'200 + kParticipantGoneMs - 1'000);
  EXPECT_TRUE(call.gone.empty()) << "nobody is taken as gone (nor the key rotated) while it is us";
  EXPECT_FALSE(call.rejoined) << "within the others' window they have not taken us as gone: nothing to rejoin yet";
  call.Run(10'000 + kParticipantGoneMs, 10'000 + kParticipantGoneMs + 1'000);
  EXPECT_TRUE(call.rejoined) << "by now they took us as gone and moved the key on: ask to join again";
  EXPECT_FALSE(call.disconnected);
}

TEST(ParticipantCutOffTest, TwoReallyDyingTogetherEndInTheWaitAloneNotALeave) {
  // The hard case: two of three die at once (no leave from either). Nothing comes back, the ask to rejoin goes
  // unanswered: each is then taken as gone and this device waits alone (V053) — it never leaves on its own for it.
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000;
  const int64_t asked = 10'000 + kParticipantGoneMs;
  call.Run(10'000, asked + kRejoinWaitMs - 1'000);
  EXPECT_EQ(call.rejoins, 1);
  EXPECT_TRUE(call.gone.empty()) << "the ask is answered within its wait if it was us";
  const ParticipantLivenessStep step = call.Run(asked + kRejoinWaitMs, asked + kRejoinWaitMs);
  EXPECT_FALSE(step.cut_off);
  EXPECT_EQ(call.gone, (Strings{"account:bob", "account:carol"}));
  EXPECT_FALSE(call.disconnected) << "the last others gone: the wait alone, as for any leave";
  call.Run(asked + kRejoinWaitMs + 1'000, asked + 120'000);
  EXPECT_EQ(call.rejoins, 1) << "asked once";
}

TEST(ParticipantCutOffTest, OthersGoingQuietAtDifferentTimesAreEachJudgedOnTheirOwn) {
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000 + kParticipantsQuietTogetherMs + 1'000;
  const ParticipantLivenessStep step = call.Run(10'000, 13'000);
  EXPECT_FALSE(step.cut_off);
  EXPECT_EQ(step.quiet, Strings{"account:bob"});
  call.Run(14'000, 10'000 + kParticipantGoneMs);
  EXPECT_EQ(call.gone, Strings{"account:bob"});
  EXPECT_FALSE(call.rejoined);
}

TEST(ParticipantCutOffTest, TheLaterOfTwoGoingQuietApartIsDimmedToo) {
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 12'500; // apart: each their own
  EXPECT_EQ(call.Run(10'000, 13'000).quiet, Strings{"account:bob"});
  EXPECT_EQ(call.Run(14'000, 16'000).quiet, (Strings{"account:bob", "account:carol"}));
  EXPECT_FALSE(call.Run(17'000, 17'000).cut_off);
}

TEST(ParticipantCutOffTest, OneOtherGoingQuietIsTheirsNotOurs) {
  // With one other, nothing tells our network from theirs: the per-participant rule (and the wait alone) holds; if it
  // was us, their frames that no longer open tell us once we are back (the next test).
  ParticipantLivenessWatch watch;
  ParticipantLivenessSample sample;
  sample.our_media_live = true;
  sample.peers.push_back({"account:bob", 10'000});
  ParticipantLivenessStep step;
  for (int64_t now = 10'000; now <= 10'000 + kParticipantGoneMs; now += 1'000) {
    sample.now_ms = now;
    step = watch.Step(sample);
    EXPECT_FALSE(step.cut_off) << now;
    EXPECT_FALSE(step.rejoin) << now;
  }
  EXPECT_EQ(step.gone, Strings{"account:bob"});
}

TEST(ParticipantCutOffTest, FramesThatNeverOpenAreTheKeyMovedOnWithoutUs) {
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000 + kParticipantsQuietTogetherMs + 1'000; // not together: only the frames say it
  call.Run(10'000, 12'000);
  const ParticipantLivenessStep shut = call.Run(13'000, 16'000, /*shut_out=*/true);
  EXPECT_TRUE(shut.cut_off) << "they are there, sealing with a key we do not hold";
  EXPECT_TRUE(shut.quiet.empty());
  EXPECT_FALSE(call.rejoined) << "a rotation's own gap, or a missed key's resend, is shorter";
  call.Run(17'000, 16'000 + kShutOutMs - 1'000, true);
  EXPECT_FALSE(call.rejoined);
  call.Run(16'000 + kShutOutMs, 16'000 + kShutOutMs, true);
  EXPECT_TRUE(call.rejoined) << "shut out for kShutOutMs once nothing opened for the quiet window";
  EXPECT_TRUE(call.gone.empty());
}

TEST(ParticipantCutOffTest, ShutOutAndUnansweredLeavesSayingSo) {
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000;
  const int64_t asked = 13'000 + kShutOutMs;
  call.Run(10'000, asked + kRejoinWaitMs - 1'000, /*shut_out=*/true);
  EXPECT_EQ(call.rejoins, 1);
  call.Run(asked + kRejoinWaitMs, asked + kRejoinWaitMs, true);
  EXPECT_EQ(call.rejoins, 2) << "they are there: ask again";
  call.Run(asked + kRejoinWaitMs + 1'000, asked + 2 * kRejoinWaitMs - 1'000, true);
  EXPECT_FALSE(call.disconnected);
  call.Run(asked + 2 * kRejoinWaitMs, asked + 2 * kRejoinWaitMs + 30'000, true);
  EXPECT_TRUE(call.disconnected) << "cut off from a call that goes on: this device leaves and says so";
  EXPECT_EQ(call.rejoins, kRejoinAsksMax);
  EXPECT_TRUE(call.gone.empty()) << "nobody it could not hear is taken as gone for it";
}

TEST(ParticipantCutOffTest, LeftAloneAndThenFramesThatDoNotOpenAsksToRejoin) {
  // We took the only other as gone (our network was down; they took us as gone too). Back online, their stream still
  // comes through the hop, under a key we never got.
  CutCall call;
  call.with_peers = false;
  call.Run(10'000, 20'000);
  EXPECT_FALSE(call.rejoined) << "alone and nothing comes: the wait alone";
  call.Run(21'000, 23'000 + kShutOutMs, /*shut_out=*/true);
  EXPECT_TRUE(call.rejoined);
}

TEST(ParticipantCutOffTest, TheGivenUpAskStillLeavesRoomForTheFramesToSayItWasUs) {
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000;
  const int64_t gave_up = 10'000 + kParticipantGoneMs + kRejoinWaitMs;
  call.Run(10'000, gave_up);
  ASSERT_EQ(call.gone.size(), 2u);
  call.with_peers = false;                                                    // their leaves landed (taken as gone)
  call.Run(gave_up + 1'000, gave_up + 3'000 + kShutOutMs, /*shut_out=*/true); // our network came back
  EXPECT_EQ(call.rejoins, 2);
}

TEST(ParticipantCutOffTest, AKeyThatArrivesLateAsksNothing) {
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000;
  call.Run(10'000, 12'000);
  call.Run(13'000, 14'000, /*shut_out=*/true); // a rotation whose key reached us late
  call.bob_ms = 14'500;
  call.carol_ms = 14'500;
  const ParticipantLivenessStep back = call.Run(15'000, 15'000);
  EXPECT_FALSE(back.cut_off);
  EXPECT_TRUE(back.cut_off_changed);
  call.Run(16'000, 30'000);
  EXPECT_FALSE(call.rejoined);
}

TEST(ParticipantCutOffTest, OurOwnRecoverySaysItInstead) {
  CutCall call;
  call.bob_ms = 10'000;
  call.carol_ms = 10'000;
  ASSERT_TRUE(call.Run(10'000, 14'000).cut_off);
  call.live = false; // our hop is being reattached: that owns the call's Reconnecting line
  const ParticipantLivenessStep paused = call.Run(15'000, 15'000);
  EXPECT_FALSE(paused.cut_off);
  EXPECT_TRUE(paused.cut_off_changed);
  call.Run(16'000, 90'000);
  EXPECT_FALSE(call.rejoined);
  EXPECT_TRUE(call.gone.empty());
}

// --- Which frames count -------------------------------------------------------------------------

TEST(PublisherFrameFreshnessTest, ANewerSeqAdvances) {
  PublisherFrameFreshness fresh;
  EXPECT_TRUE(fresh.Advances(7, 0, 0xAA, 100));
  EXPECT_TRUE(fresh.Advances(7, 0, 0xAA, 101));
  EXPECT_TRUE(fresh.Advances(7, 0, 0xAA, 105)) << "a loss in between is fine";
}

TEST(PublisherFrameFreshnessTest, AReplayOrAnOlderSeqDoesNot) {
  PublisherFrameFreshness fresh;
  ASSERT_TRUE(fresh.Advances(7, 0, 0xAA, 100));
  ASSERT_TRUE(fresh.Advances(7, 0, 0xAA, 101));
  EXPECT_FALSE(fresh.Advances(7, 0, 0xAA, 101)) << "the hop repeating a frame";
  EXPECT_FALSE(fresh.Advances(7, 0, 0xAA, 90)) << "an old frame";
}

TEST(PublisherFrameFreshnessTest, EachChannelAndSenderInstanceCountsOnItsOwn) {
  PublisherFrameFreshness fresh;
  ASSERT_TRUE(fresh.Advances(7, 0, 0xAA, 100));
  EXPECT_TRUE(fresh.Advances(7, 1, 0xAA, 5)) << "video numbers its own frames";
  EXPECT_TRUE(fresh.Advances(7, 0, 0xBB, 1)) << "a restarted sender starts over";
  EXPECT_FALSE(fresh.Advances(7, 0, 0xAA, 100)) << "the old instance's frames are still old";
  EXPECT_TRUE(fresh.Advances(8, 0, 0xAA, 1)) << "another stream";
}

TEST(PublisherFrameFreshnessTest, TheSeqWrapsAround) {
  PublisherFrameFreshness fresh;
  ASSERT_TRUE(fresh.Advances(7, 0, 0xAA, 0xFFFFFFFFu));
  EXPECT_TRUE(fresh.Advances(7, 0, 0xAA, 0));
  EXPECT_FALSE(fresh.Advances(7, 0, 0xAA, 0xFFFFFFFEu));
}

TEST(PublisherFrameFreshnessTest, AForgottenStreamStartsOver) {
  PublisherFrameFreshness fresh;
  ASSERT_TRUE(fresh.Advances(7, 0, 0xAA, 100));
  fresh.Forget(7);
  EXPECT_TRUE(fresh.Advances(7, 0, 0xAA, 50));
}

TEST(PublisherFrameFreshnessTest, OnlyTheLatestSenderInstancesAreKept) {
  PublisherFrameFreshness fresh;
  for (uint64_t instance = 1; instance <= PublisherFrameFreshness::kInstancesKept; ++instance) {
    ASSERT_TRUE(fresh.Advances(7, 0, instance, 10));
  }
  ASSERT_TRUE(fresh.Advances(7, 0, 2, 11)); // instance 1 is now the least recent
  ASSERT_TRUE(fresh.Advances(7, 0, 99, 1)); // a new one pushes it out
  EXPECT_FALSE(fresh.Advances(7, 0, 2, 11)) << "kept";
  EXPECT_TRUE(fresh.Advances(7, 0, 1, 10)) << "forgotten: bounded memory per stream";
}

} // namespace
} // namespace pbr
