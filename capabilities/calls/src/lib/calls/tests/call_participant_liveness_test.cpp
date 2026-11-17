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
