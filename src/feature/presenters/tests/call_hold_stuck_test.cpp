#include "feature/presenters/CallHoldStuck.h"
#include "feature/presenters/CallPresenter.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// "The OS took the call's audio and has not given it back" (B217): a hold that outlasts 12 s in front says once what
// helps. The timer counts only held + in front + not reconnecting, and is sure before it speaks.

using End = CallHoldStuck::End;

struct Feed {
  CallHoldStuck timer;
  int64_t now = 100'000;
  int edges = 0;
  int ends = 0;
  CallHoldStuck::Result last;
  CallHoldStuck::Result last_end;

  /** Ticks every `step_ms` for `seconds`; returns whether the last tick said stuck. */
  bool Run(const int64_t seconds, const bool held, const bool foreground = true, const bool reconnecting = false,
           const int64_t step_ms = 500) {
    for (int64_t t = step_ms; t <= seconds * 1000; t += step_ms) {
      now += step_ms;
      Tick(held, foreground, reconnecting);
    }
    return last.stuck;
  }
  bool Tick(const bool held, const bool foreground = true, const bool reconnecting = false) {
    last = timer.Update({.held = held, .foreground = foreground, .reconnecting = reconnecting, .now_ms = now});
    edges += last.became_stuck ? 1 : 0;
    if (last.ended) {
      ++ends;
      last_end = last;
    }
    return last.stuck;
  }
};

TEST(CallHoldStuckTest, AShortHoldIsNeverStuckAndTheTwelfthSecondIs) {
  Feed f;
  f.Tick(true);
  EXPECT_FALSE(f.Run(11, true)) << "11 s";
  f.now += 400;
  EXPECT_FALSE(f.Tick(true)) << "11.4 s";
  f.now += 500;
  EXPECT_FALSE(f.Tick(true)) << "11.9 s";
  f.now += 100;
  EXPECT_TRUE(f.Tick(true)) << "12 s";
  EXPECT_EQ(f.edges, 1);
  EXPECT_TRUE(f.Run(30, true));
  EXPECT_EQ(f.edges, 1) << "the rising edge comes once";
}

TEST(CallHoldStuckTest, ANormalSiriHoldNeverShowsAndLeavesNoEndEvent) {
  Feed f;
  f.Tick(false);
  f.Run(7, true);
  EXPECT_FALSE(f.last.stuck);
  f.Run(5, false);
  EXPECT_EQ(f.edges, 0);
  EXPECT_EQ(f.ends, 0) << "a spell that never showed is not reported";
}

TEST(CallHoldStuckTest, ABackgroundTickNeverGivesTheRisingEdgeAndTheCountIsKept) {
  Feed f;
  f.Tick(true);
  f.Run(6, true);
  // 60 s with the app away (ticks keep coming, or one gap): nothing counts, nothing shows.
  EXPECT_FALSE(f.Run(60, true, /*foreground=*/false));
  EXPECT_EQ(f.edges, 0);
  // Back in front: the count (6 s) goes on, and the first appearance waits the grace after the last background tick.
  EXPECT_FALSE(f.Run(5, true));
  EXPECT_TRUE(f.Run(2, true)) << "about 12 s counted";
  EXPECT_EQ(f.edges, 1);
}

TEST(CallHoldStuckTest, PullingDownTheShadeDoesNotLoseTheCount) {
  Feed f;
  f.Tick(true);
  int shown_at_cycle = -1;
  for (int i = 0; i < 12; ++i) { // 5 s in front, 2 s away, again and again
    f.Run(5, true);
    if (f.last.stuck && shown_at_cycle < 0) {
      shown_at_cycle = i;
    }
    f.Run(2, true, /*foreground=*/false);
  }
  EXPECT_GE(shown_at_cycle, 0) << "12 s of count was reached in spite of the trips";
  EXPECT_LE(shown_at_cycle, 3);
  EXPECT_EQ(f.edges, 1);
}

TEST(CallHoldStuckTest, TheFirstAppearanceWaitsThreeSecondsAfterReturningToFront) {
  Feed f;
  f.Tick(true);
  f.Run(11, true);
  f.now += 400;
  f.Tick(true, false); // away for a moment, 11.4 s counted
  f.now += 100;
  f.Tick(true, false);
  f.Run(2, true); // back: the count passes 12 s, but it is under 3 s since the last background tick
  EXPECT_FALSE(f.last.stuck);
  EXPECT_EQ(f.edges, 0);
  f.Run(2, true);
  EXPECT_TRUE(f.last.stuck) << "3 s after the return";
  EXPECT_EQ(f.edges, 1);
}

TEST(CallHoldStuckTest, OnceShownItStaysThroughAShortTripToTheBackground) {
  Feed f;
  f.Tick(true);
  f.Run(13, true);
  ASSERT_TRUE(f.last.stuck);
  EXPECT_FALSE(f.Tick(true, false)) << "away: not shown (display only)";
  f.now += 500;
  EXPECT_TRUE(f.Tick(true)) << "back in front: shown at once, no new grace";
  EXPECT_EQ(f.edges, 1);
  EXPECT_EQ(f.ends, 0);
}

TEST(CallHoldStuckTest, ReconnectingDoesNotCountAndTheTextComesBackThreeSecondsAfterItEnds) {
  Feed f;
  f.Tick(true);
  f.Run(11, true); // 11 s counted
  f.Run(30, true, true, /*reconnecting=*/true);
  EXPECT_FALSE(f.last.stuck);
  EXPECT_EQ(f.edges, 0);
  EXPECT_EQ(f.timer.counted_ms(), 11'000) << "paused, kept";
  // The reconnect ends: 1 more counted second makes 12 s, but the grace after the reconnect has not gone by.
  EXPECT_FALSE(f.Tick(true)) << "not at the instant the reconnect ends";
  EXPECT_FALSE(f.Run(2, true)) << "12 s counted, 2.5 s after the reconnect";
  EXPECT_EQ(f.edges, 0);
  EXPECT_TRUE(f.Run(1, true)) << "3 s after it";
  EXPECT_EQ(f.edges, 1);
}

TEST(CallHoldStuckTest, ReconnectingAfterItShowedGivesWayThenReturnsAfterTheGrace) {
  Feed f;
  f.Tick(true);
  f.Run(13, true);
  ASSERT_TRUE(f.last.stuck);
  EXPECT_FALSE(f.Run(5, true, true, true));
  EXPECT_FALSE(f.Tick(true)) << "the instant it ends";
  f.Run(2, true);
  EXPECT_FALSE(f.last.stuck);
  EXPECT_TRUE(f.Run(2, true));
  EXPECT_EQ(f.edges, 1) << "the same spell";
}

TEST(CallHoldStuckTest, AHoldThatComesAndGoesWithinTwoSecondsKeepsCounting) {
  Feed f;
  f.Tick(true);
  f.Run(6, true);
  f.Run(1, false); // the retry chain got it back for a second
  EXPECT_FALSE(f.Run(5, true)) << "about 11 s counted: not yet";
  EXPECT_EQ(f.edges, 0);
  EXPECT_TRUE(f.Run(2, true)) << "6 s more after the gap";
  EXPECT_EQ(f.edges, 1);
}

TEST(CallHoldStuckTest, AHoldThatEndedForThreeSecondsStartsFromZero) {
  Feed f;
  f.Tick(true);
  f.Run(6, true);
  f.Run(3, false);
  f.Run(8, true);
  EXPECT_FALSE(f.last.stuck) << "8 s from zero";
  EXPECT_TRUE(f.Run(5, true));
}

TEST(CallHoldStuckTest, ATickGapOrAClockGoingBackAddsNothingAndClearsNothing) {
  Feed f;
  f.Tick(true);
  f.Run(6, true);
  const int64_t before = f.timer.counted_ms();
  f.now += 600'000; // the UI was suspended
  f.Tick(true);
  EXPECT_EQ(f.timer.counted_ms(), before);
  EXPECT_FALSE(f.last.stuck);
  f.now -= 100'000; // the clock went back
  f.Tick(true);
  EXPECT_EQ(f.timer.counted_ms(), before);
  EXPECT_FALSE(f.last.stuck);
  EXPECT_EQ(f.edges, 0);
}

TEST(CallHoldStuckTest, TheHoldEndingEndsTheSpellOnceAndTheNextHoldStartsFromZero) {
  Feed f;
  f.Tick(true);
  f.Run(14, true);
  ASSERT_TRUE(f.last.stuck);
  EXPECT_FALSE(f.Tick(false)) << "the text is gone on the tick the hold ends";
  EXPECT_EQ(f.ends, 1);
  EXPECT_EQ(f.last_end.end, End::Recovered);
  EXPECT_GE(f.last_end.counted_ms, 13'000);
  f.Run(5, false);
  EXPECT_EQ(f.ends, 1) << "once per spell";
  // A second hold: from zero, another edge, another end.
  f.Run(13, true);
  EXPECT_TRUE(f.last.stuck);
  EXPECT_EQ(f.edges, 2);
  f.Tick(false);
  EXPECT_EQ(f.ends, 2);
}

TEST(CallHoldStuckTest, TheCallEndingEndsAShownSpellOnceWithCallEnded) {
  Feed f;
  f.Tick(true);
  f.Run(14, true);
  const auto end = f.timer.Finish();
  EXPECT_TRUE(end.ended);
  EXPECT_EQ(end.end, End::CallEnded);
  EXPECT_GE(end.counted_ms, 13'000);
  EXPECT_FALSE(f.timer.Finish().ended) << "once";
  EXPECT_EQ(f.timer.counted_ms(), 0);

  Feed g; // never shown: nothing to report
  g.Tick(true);
  g.Run(5, true);
  EXPECT_FALSE(g.timer.Finish().ended);
}

TEST(CallHoldStuckTest, AUserMuteIsNotAHoldAndResetStartsOver) {
  Feed f;
  f.Tick(false);
  EXPECT_FALSE(f.Run(60, false)) << "held is false whatever the mic says";
  EXPECT_EQ(f.edges, 0);
  f.Tick(true);
  f.Run(6, true);
  f.timer.Reset();
  EXPECT_EQ(f.timer.counted_ms(), 0);
  EXPECT_FALSE(f.timer.Finish().ended);
}

// --- The presenter's side: the real Callbacks' app_foreground port feeds TrackHoldStuck (CallController binds it).

struct PresenterFeed {
  CallPresenter presenter;
  bool held = false;
  bool foreground = true;
  int64_t now = 100'000;
  CallScreen screen;
  PresenterFeed() {
    CallPresenter::Callbacks callbacks;
    callbacks.on_hold_here = [this]() { return held; };
    callbacks.app_foreground = [this]() { return foreground; };
    presenter.BindCallbacks(std::move(callbacks));
    screen.kind = CallScreen::Kind::InCall;
    screen.status = CallScreen::Status::Connected;
  }
  bool Run(const int64_t seconds, const bool reconnecting = false) {
    CallMediaFacts facts;
    for (int64_t t = 0; t < seconds * 2; ++t) {
      now += 500;
      facts = CallMediaFacts{};
      facts.call_id = "call-a";
      facts.now_ms = now;
      facts.local_on_hold = held; // what ReadMediaFacts writes from on_hold_here
      facts.media_status = reconnecting ? CallMediaStatus::Reconnecting : CallMediaStatus::None;
      presenter.TrackHoldStuck(facts, screen);
    }
    return facts.hold_stuck;
  }
};

TEST(CallHoldStuckPresenterTest, AHoldInFrontForTwelveSecondsSetsTheFact) {
  PresenterFeed f;
  f.held = true;
  EXPECT_FALSE(f.Run(8));
  EXPECT_TRUE(f.Run(8));
  f.held = false;
  EXPECT_FALSE(f.Run(1)) << "the hold ended: the fact goes";
}

TEST(CallHoldStuckPresenterTest, TheAppInTheBackgroundNeverShowsAndAnUnboundForegroundCountsAsInFront) {
  PresenterFeed f;
  f.held = true;
  f.foreground = false;
  EXPECT_FALSE(f.Run(60));

  CallPresenter presenter; // no app_foreground port: always in front
  CallPresenter::Callbacks callbacks;
  callbacks.on_hold_here = []() { return true; };
  presenter.BindCallbacks(std::move(callbacks));
  CallScreen screen;
  screen.kind = CallScreen::Kind::InCall;
  CallMediaFacts facts;
  for (int i = 1; i <= 40; ++i) {
    facts = CallMediaFacts{};
    facts.call_id = "call-b";
    facts.now_ms = 1'000 + i * 500;
    facts.local_on_hold = true;
    presenter.TrackHoldStuck(facts, screen);
  }
  EXPECT_TRUE(facts.hold_stuck);
}

TEST(CallHoldStuckPresenterTest, ReconnectingIsReadFromTheMediaFactsAndNeverShows) {
  PresenterFeed f;
  f.held = true;
  EXPECT_FALSE(f.Run(60, /*reconnecting=*/true));
  EXPECT_FALSE(f.Run(2)) << "the grace after the reconnect";
  EXPECT_TRUE(f.Run(14));
}

TEST(CallHoldStuckPresenterTest, AnotherCallStartsFromZero) {
  PresenterFeed f;
  f.held = true;
  f.Run(8);
  CallMediaFacts facts;
  for (int i = 1; i <= 16; ++i) { // 8 s on another call: 16 s in all would be stuck, 8 s is not
    facts = CallMediaFacts{};
    facts.call_id = "call-b";
    facts.now_ms = f.now + i * 500;
    facts.local_on_hold = true;
    f.presenter.TrackHoldStuck(facts, f.screen);
  }
  EXPECT_FALSE(facts.hold_stuck);
}

} // namespace
} // namespace pbr
