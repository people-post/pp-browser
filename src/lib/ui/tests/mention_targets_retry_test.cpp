#include "lib/ui/MentionTargetsRetry.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(MentionTargetsRetryTest, NothingPendingNeverDue) {
  MentionTargetsRetry retry;
  EXPECT_FALSE(retry.Due(1'000'000));
  retry.Record(/*unknown=*/false, 0);
  EXPECT_FALSE(retry.Due(1'000'000));
}

TEST(MentionTargetsRetryTest, UnknownRetriesAtMostOncePerSecond) {
  MentionTargetsRetry retry;
  retry.Record(true, 10'000);
  EXPECT_FALSE(retry.Due(10'000));
  EXPECT_FALSE(retry.Due(10'000 + kMentionTargetsRetryGapMs - 1));
  EXPECT_TRUE(retry.Due(10'000 + kMentionTargetsRetryGapMs));
  retry.Record(true, 11'500); // the retry failed again: wait again
  EXPECT_FALSE(retry.Due(11'600));
  EXPECT_TRUE(retry.Due(12'500));
}

TEST(MentionTargetsRetryTest, AnAnswerEndsIt) {
  MentionTargetsRetry retry;
  retry.Record(true, 0);
  retry.Record(false, 2'000);
  EXPECT_FALSE(retry.Due(100'000));
}

} // namespace
} // namespace pbr
