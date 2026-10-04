#include "gui/chat/AttachmentBackfillPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

// The backfill reads the whole thread: on opening a thread, then at most once a minute while it is open.
TEST(AttachmentBackfillPolicyTest, OnThreadOpenThenAtMostOncePerInterval) {
  const Clock::time_point opened = Clock::time_point{} + std::chrono::hours(1);
  EXPECT_TRUE(ShouldBackfillAttachments(/*thread_changed=*/true, opened, opened));

  // Syncs for new messages right after do not read the thread again...
  EXPECT_FALSE(ShouldBackfillAttachments(false, opened + std::chrono::seconds(1), opened));
  EXPECT_FALSE(ShouldBackfillAttachments(false, opened + kAttachmentBackfillInterval - std::chrono::seconds(1), opened));
  // ...but a failed download in an open thread is retried within the interval.
  EXPECT_TRUE(ShouldBackfillAttachments(false, opened + kAttachmentBackfillInterval, opened));
}

} // namespace
} // namespace pbr
