#include "domain/messaging/AttachmentDownloadPolicy.h"
#include "common/chat/MessagingLimits.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AttachmentDownloadPolicyTest, SmartAutoEnqueuesSmallFilesOnly) {
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, 1024, false));
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kMaxChatAttachmentPlaintextBytes, false));
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kMaxChatAttachmentPlaintextBytes + 1, false));
}

TEST(AttachmentDownloadPolicyTest, BacklogDrainOverridesSmart) {
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kMaxChatAttachmentPlaintextBytes + 1, true));
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::OnDemand, 1024, true));
}

TEST(AttachmentDownloadPolicyTest, WireRoundTrip) {
  EXPECT_EQ(AttachmentDownloadPolicyFromString("smart"), AttachmentDownloadPolicy::Smart);
  EXPECT_EQ(AttachmentDownloadPolicyToString(AttachmentDownloadPolicy::AlwaysAuto), "always_auto");
}

} // namespace
} // namespace pbr
