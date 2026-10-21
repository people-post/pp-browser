#include "feature/calls/SeedParkOutcome.h"

#include <gtest/gtest.h>

using pbr::SeedParkOutcome;

TEST(SeedParkOutcomeTest, TimedOutParkMeansSeedsUnreachable) {
  EXPECT_TRUE(SeedParkOutcome::SeedUnreachable(false, 12000, 12000, 1, 1));
  EXPECT_TRUE(SeedParkOutcome::SeedUnreachable(false, 12000 - SeedParkOutcome::kTimeoutSlackMs, 12000, 1, 1));
}

TEST(SeedParkOutcomeTest, SuccessIsNotUnreachable) {
  EXPECT_FALSE(SeedParkOutcome::SeedUnreachable(true, 12000, 12000, 1, 1));
}

TEST(SeedParkOutcomeTest, EarlyFailureIsNotATimeout) {
  // No seeds configured, mesh down or abandoned on stop all settle well before the deadline.
  EXPECT_FALSE(SeedParkOutcome::SeedUnreachable(false, 0, 12000, 1, 1));
  EXPECT_FALSE(SeedParkOutcome::SeedUnreachable(false, 12000 - SeedParkOutcome::kTimeoutSlackMs - 1, 12000, 1, 1));
}

TEST(SeedParkOutcomeTest, StaleParkFromAnEarlierAttemptIsIgnored) {
  // The user hung up or retried (epoch moved) while the park was pending; its timeout must not
  // label the new attempt.
  EXPECT_FALSE(SeedParkOutcome::SeedUnreachable(false, 12000, 12000, 1, 2));
}
