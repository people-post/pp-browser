#include "lib/media/ResumeRetryChain.h"

#include <gtest/gtest.h>

#include <chrono>

namespace pbr {
namespace {

using namespace std::chrono_literals;
using Chain = ResumeRetryChain;
using AppState = Chain::AppState;
using Action = Chain::Action;
using TryResult = Chain::TryResult;
using StopReason = Chain::StopReason;

const Chain::TimePoint kT0 = Chain::TimePoint{} + 1000s;

// The first interruption starts a chain; another while it runs does not start a second.
TEST(ResumeRetryChainTest, FirstBeganStartsAChainAndASecondDoesNotStartAnother) {
  Chain chain;
  EXPECT_FALSE(chain.Running());
  EXPECT_TRUE(chain.OnBegan(kT0));
  EXPECT_TRUE(chain.Running());
  EXPECT_FALSE(chain.OnBegan(kT0 + 2s)) << "one chain at a time";
  EXPECT_TRUE(chain.Running());
}

TEST(ResumeRetryChainTest, TickStopsWhenTheSessionEnded) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  const auto tick = chain.OnTick(/*session_active=*/false, /*interrupted=*/true, AppState::Active);
  EXPECT_EQ(tick.action, Action::Stop);
  EXPECT_EQ(tick.stop, StopReason::SessionEnded);
  EXPECT_FALSE(chain.Running());
}

TEST(ResumeRetryChainTest, TickStopsWhenNoLongerInterrupted) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  const auto tick = chain.OnTick(true, /*interrupted=*/false, AppState::Active);
  EXPECT_EQ(tick.action, Action::Stop);
  EXPECT_EQ(tick.stop, StopReason::ResumedElsewhere);
  EXPECT_FALSE(chain.Running());
}

// Siri / a call screen / Control Center over the app: wait, keep the chain.
TEST(ResumeRetryChainTest, TickSkipsWhileInactiveAndTheChainContinues) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  const auto tick = chain.OnTick(true, true, AppState::Inactive);
  EXPECT_EQ(tick.action, Action::Skip);
  EXPECT_TRUE(tick.log_skip) << "the first skip is logged";
  EXPECT_TRUE(chain.Running());
  EXPECT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
}

TEST(ResumeRetryChainTest, TickTriesWhenActiveBackgroundOrUnknown) {
  for (const AppState state : {AppState::Active, AppState::Background, AppState::Unknown}) {
    Chain chain;
    ASSERT_TRUE(chain.OnBegan(kT0));
    EXPECT_EQ(chain.OnTick(true, true, state).action, Action::Try) << Chain::Word(state);
    EXPECT_TRUE(chain.Running());
  }
}

TEST(ResumeRetryChainTest, OkStopsAndGivesTheRetryMetric) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  ASSERT_TRUE(chain.OnTryResult(TryResult::Failed, kT0 + 1500ms).reschedule);
  ASSERT_EQ(chain.OnTick(true, true, AppState::Inactive).action, Action::Skip) << "a skip is not a try";
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  const auto outcome = chain.OnTryResult(TryResult::Ok, kT0 + 4500ms);
  EXPECT_FALSE(outcome.reschedule);
  EXPECT_EQ(outcome.stop, StopReason::Resumed);
  EXPECT_EQ(outcome.tries, 2);
  EXPECT_EQ(outcome.waited, 4500ms);
  EXPECT_FALSE(chain.Running());
}

TEST(ResumeRetryChainTest, FailedReschedules) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  const auto outcome = chain.OnTryResult(TryResult::Failed, kT0 + 1500ms);
  EXPECT_TRUE(outcome.reschedule);
  EXPECT_EQ(outcome.failures, 1);
  EXPECT_TRUE(chain.Running());
}

// A newer activation took over meanwhile: look again at the next tick.
TEST(ResumeRetryChainTest, StaleReschedules) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  const auto outcome = chain.OnTryResult(TryResult::Stale, kT0 + 1500ms);
  EXPECT_TRUE(outcome.reschedule);
  EXPECT_FALSE(outcome.log_failure);
  EXPECT_TRUE(chain.Running());
}

TEST(ResumeRetryChainTest, GoneStops) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  const auto outcome = chain.OnTryResult(TryResult::Gone, kT0 + 1500ms);
  EXPECT_FALSE(outcome.reschedule);
  EXPECT_EQ(outcome.stop, StopReason::SessionEnded);
  EXPECT_FALSE(chain.Running());
}

TEST(ResumeRetryChainTest, AlreadyStops) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  const auto outcome = chain.OnTryResult(TryResult::Already, kT0 + 1500ms);
  EXPECT_FALSE(outcome.reschedule);
  EXPECT_EQ(outcome.stop, StopReason::ResumedElsewhere);
  EXPECT_FALSE(chain.Running());
}

// B217: resumed, then the OS took the audio again two seconds later — that interruption needs its own chain.
TEST(ResumeRetryChainTest, ABeganAfterAStopStartsANewChain) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  ASSERT_EQ(chain.OnTryResult(TryResult::Ok, kT0 + 1500ms).stop, StopReason::Resumed);
  ASSERT_FALSE(chain.Running());
  EXPECT_TRUE(chain.OnBegan(kT0 + 3500ms));
  EXPECT_TRUE(chain.Running());
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  const auto outcome = chain.OnTryResult(TryResult::Ok, kT0 + 5000ms);
  EXPECT_EQ(outcome.tries, 1) << "counted from the new interruption";
  EXPECT_EQ(outcome.waited, 1500ms);
}

TEST(ResumeRetryChainTest, FailureLogIsThrottled) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  for (int n = 1; n <= 21; ++n) {
    ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
    const auto outcome = chain.OnTryResult(TryResult::Failed, kT0 + n * 1500ms);
    EXPECT_EQ(outcome.failures, n);
    EXPECT_EQ(outcome.log_failure, n == 1 || n == 11 || n == 21) << "failure " << n;
  }
}

TEST(ResumeRetryChainTest, SinceBeganIsZeroBeforeAnyInterruption) {
  Chain chain;
  EXPECT_EQ(chain.SinceBegan(kT0), 0ms);
  ASSERT_TRUE(chain.OnBegan(kT0));
  EXPECT_EQ(chain.SinceBegan(kT0 + 800ms), 800ms);
}

// A resume with no interruption recorded (a media-services reset) reports no wait, not a stale one.
TEST(ResumeRetryChainTest, SinceBeganIsZeroAfterClearBegan) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  chain.ClearBegan();
  EXPECT_EQ(chain.SinceBegan(kT0 + 800ms), 0ms);
  EXPECT_TRUE(chain.Running()) << "only the time is forgotten";
}

TEST(ResumeRetryChainTest, OkForgetsWhenTheInterruptionBegan) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  ASSERT_EQ(chain.OnTryResult(TryResult::Ok, kT0 + 1500ms).waited, 1500ms);
  EXPECT_EQ(chain.SinceBegan(kT0 + 60s), 0ms);
  ASSERT_TRUE(chain.OnBegan(kT0 + 70s));
  EXPECT_EQ(chain.SinceBegan(kT0 + 71s), 1s) << "a new interruption records its own time";
}

// A "began" while a chain runs (the OS took the audio again): same chain, counters and wait start again.
TEST(ResumeRetryChainTest, ABeganWhileRunningRestartsTheCounters) {
  Chain chain;
  ASSERT_TRUE(chain.OnBegan(kT0));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  ASSERT_EQ(chain.OnTryResult(TryResult::Failed, kT0 + 1500ms).failures, 1);
  EXPECT_FALSE(chain.OnBegan(kT0 + 2s));
  ASSERT_EQ(chain.OnTick(true, true, AppState::Active).action, Action::Try);
  const auto outcome = chain.OnTryResult(TryResult::Ok, kT0 + 3500ms);
  EXPECT_EQ(outcome.tries, 1);
  EXPECT_EQ(outcome.waited, 1500ms);
}

} // namespace
} // namespace pbr
