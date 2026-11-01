#include "common/chat/AttachmentDownloadPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr uint64_t kSmartMax = 4000000;

TEST(AttachmentDownloadPolicyTest, SmartAutoEnqueuesSmallFilesOnly) {
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, 1024, false, kSmartMax));
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kSmartMax, false, kSmartMax));
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kSmartMax + 1, false, kSmartMax));
}

TEST(AttachmentDownloadPolicyTest, SmartThresholdIsTheCallersLimit) {
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, 2000, false, 1000));
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, 2000, false, 2000));
}

TEST(AttachmentDownloadPolicyTest, BacklogDrainOverridesSmart) {
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kSmartMax + 1, true, kSmartMax));
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::OnDemand, 1024, true, kSmartMax));
}

TEST(AttachmentDownloadPolicyTest, WireRoundTrip) {
  EXPECT_EQ(AttachmentDownloadPolicyFromString("smart"), AttachmentDownloadPolicy::Smart);
  EXPECT_EQ(AttachmentDownloadPolicyToString(AttachmentDownloadPolicy::AlwaysAuto), "always_auto");
}

} // namespace
} // namespace pbr
