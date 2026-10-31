#include "domain/attachments/AttachmentFetchWorkflow.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "common/Utilities.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>

namespace pbr {
namespace {

class ReceiveLimitsTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_receive_limits_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    workflow_.SetProfileDataDir(dir_.string());
    workflow_.SetProfileId("test");
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  static std::vector<uint8_t> Hash(const uint8_t seed) { return std::vector<uint8_t>(kAttachmentContentHashSize, seed); }

  static ThreadMessage AttachmentMessage(const std::string& id, const std::string& sender, const uint8_t seed,
                                         const uint64_t byte_length) {
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/blob/" + id; // fetchable, so only the limits decide
    fields.mime = "application/pdf";
    fields.filename = id + ".pdf";
    fields.byte_length = byte_length;
    fields.content_hash = Hash(seed);
    fields.content_key.assign(kSessionKeySize, 1);
    fields.blob_nonce.assign(kAeadNonceSize, 2);
    ThreadMessage message;
    message.id = id;
    message.thread_id = "t1";
    message.sender_contact_id = sender;
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return message;
  }

  std::filesystem::path dir_;
  AttachmentFetchWorkflow workflow_;
};

TEST_F(ReceiveLimitsTest, DeclaredSizeAboveTheReceiveLimitIsTooLargeAndNeverQueued) {
  AttachmentLimits limits;
  limits.max_receive_bytes = 1000;
  workflow_.SetLimits(limits);
  EXPECT_NE(workflow_.StateFor("t1", Hash(1), 1000), AttachmentFetchWorkflow::DownloadState::TooLarge);
  EXPECT_EQ(workflow_.StateFor("t1", Hash(1), 1001), AttachmentFetchWorkflow::DownloadState::TooLarge);

  // Even a tap on "Download" (force) does not start it.
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("m1", "alice", 1, 5000), /*force=*/true);
  EXPECT_EQ(workflow_.StateFor("t1", Hash(1), 5000), AttachmentFetchWorkflow::DownloadState::TooLarge);
}

TEST_F(ReceiveLimitsTest, ShippingReceiveLimitIsFourMegabytes) {
  const AttachmentLimits limits = workflow_.Limits();
  EXPECT_EQ(limits.max_receive_bytes, kDefaultAttachmentReceiveMaxBytes);
  EXPECT_EQ(workflow_.StateFor("t1", Hash(2), 4'000'001), AttachmentFetchWorkflow::DownloadState::TooLarge);
}

TEST_F(ReceiveLimitsTest, ASenderOverItsMinuteLimitStaysADownloadRowButAnotherSenderDoesNot) {
  AttachmentLimits limits;
  limits.auto_download_max_count_per_minute = 2;
  workflow_.SetLimits(limits);
  int64_t now_ms = 10'000;
  workflow_.SetClockForTest([&now_ms]() { return now_ms; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });

  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 100));
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 100));
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("alice", 100));
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("bob", 100)) << "another sender is not affected";
  now_ms += 61'000;
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 100)) << "the minute has passed";
}

TEST_F(ReceiveLimitsTest, BytesPerMinuteLimitIsPerSender) {
  AttachmentLimits limits;
  limits.auto_download_max_bytes_per_minute = 20'000'000;
  workflow_.SetLimits(limits);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 15'000'000));
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("alice", 6'000'000));
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("bob", 6'000'000));
}

TEST_F(ReceiveLimitsTest, AutomaticDownloadsStopOnceTheStoreIsOverTheDiskCap) {
  AttachmentLimits limits;
  limits.auto_download_disk_cap_bytes = 1'000'000'000;
  workflow_.SetLimits(limits);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  uint64_t disk = 999'999'999;
  workflow_.SetDiskUsageProvider([&disk]() { return disk; });
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 100));
  disk = 1'000'000'000;
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("alice", 100));
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("bob", 100)) << "the cap is for everyone";
}

TEST_F(ReceiveLimitsTest, ThrottledAttachmentShowsDownloadAndNeverStartsByItself) {
  AttachmentLimits limits;
  limits.auto_download_max_count_per_minute = 0; // nothing may start by itself
  workflow_.SetLimits(limits);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });

  workflow_.EnqueueFromMessage("t1", AttachmentMessage("m2", "alice", 3, 2000));
  EXPECT_EQ(workflow_.StateFor("t1", Hash(3), 2000), AttachmentFetchWorkflow::DownloadState::Pending);
  // Looking again (reopening the thread) does not try again either: still a row with "Download".
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("m2", "alice", 3, 2000));
  EXPECT_EQ(workflow_.StateFor("t1", Hash(3), 2000), AttachmentFetchWorkflow::DownloadState::Pending);
}

TEST_F(ReceiveLimitsTest, FullDiskStopsAutomaticDownloadsToo) {
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{2'000'000'000}; });
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("m3", "bob", 4, 2000));
  EXPECT_EQ(workflow_.StateFor("t1", Hash(4), 2000), AttachmentFetchWorkflow::DownloadState::Pending);
}

TEST_F(ReceiveLimitsTest, SmartPolicyStillOnlyStartsSmallFilesByItself) {
  AttachmentLimits limits;
  limits.auto_download_max_bytes = 1000;
  limits.max_receive_bytes = 10'000;
  workflow_.SetLimits(limits);
  EXPECT_EQ(workflow_.StateFor("t1", Hash(5), 5000), AttachmentFetchWorkflow::DownloadState::Pending)
      << "above the automatic size: a row with the size and Download";
}

TEST_F(ReceiveLimitsTest, AnUndeclaredSizeIsNeverAnAutomaticDownloadAndCostsNoBudget) {
  AttachmentLimits limits;
  limits.auto_download_max_count_per_minute = 1;
  workflow_.SetLimits(limits);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto); // even "always" does not start a size of 0

  workflow_.EnqueueFromMessage("t1", AttachmentMessage("z1", "alice", 6, 0));
  EXPECT_EQ(workflow_.StateFor("t1", Hash(6), 0), AttachmentFetchWorkflow::DownloadState::Pending);
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 100)) << "the zero-size one spent nothing";
}

TEST_F(ReceiveLimitsTest, ALookAtAnAlreadyQueuedAttachmentDoesNotSpendTheBudgetAgain) {
  AttachmentLimits limits;
  limits.auto_download_max_count_per_minute = 2;
  workflow_.SetLimits(limits);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });

  // First look queues it (no worker runs in this test, so it stays queued) and spends one of the two.
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("q1", "alice", 7, 2000));
  EXPECT_EQ(workflow_.StateFor("t1", Hash(7), 2000), AttachmentFetchWorkflow::DownloadState::Downloading);
  // Reopening the thread looks again, many times.
  for (int i = 0; i < 5; ++i) {
    workflow_.EnqueueFromMessage("t1", AttachmentMessage("q1", "alice", 7, 2000));
  }
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 100)) << "one of the two is still there";
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("alice", 100)) << "and now both are spent";
}

TEST_F(ReceiveLimitsTest, TheStoreScanRunsOutsideTheWorkflowLock) {
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  // The scan asks for a download state, which takes the workflow's lock: if the scan were run under it, this
  // would deadlock instead of finishing.
  workflow_.SetStoreScanForTest([this]() {
    (void)workflow_.StateFor("t1", Hash(8), 100);
    return uint64_t{0};
  });
  auto done = std::async(std::launch::async, [this]() { return workflow_.TryAutoDownloadBudget("alice", 100); });
  ASSERT_EQ(done.wait_for(std::chrono::seconds(5)), std::future_status::ready) << "deadlocked on the scan";
  EXPECT_TRUE(done.get());
}

TEST_F(ReceiveLimitsTest, DirectPushesFollowTheSamePaceAndTheDiskCapCountsPendingCiphertext) {
  AttachmentLimits limits;
  limits.auto_download_max_count_per_minute = 10;
  limits.auto_download_disk_cap_bytes = 6000;
  workflow_.SetLimits(limits);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  // Real scan: 5000 bytes of pushed ciphertext waiting in a thread.
  ASSERT_TRUE(SavePendingAttachmentCiphertext(dir_.string(), "t1", Hash(9), std::vector<uint8_t>(5000, 1), {}));

  EXPECT_TRUE(workflow_.TryAcceptDirectPush("alice", 500));  // 5000 waiting
  EXPECT_TRUE(workflow_.TryAcceptDirectPush("alice", 500));  // 5500
  EXPECT_FALSE(workflow_.TryAcceptDirectPush("alice", 500)) << "6000 is the cap: the pending ciphertext counts";
  EXPECT_FALSE(workflow_.TryAcceptDirectPush("bob", 500)) << "the cap is for everyone";

  AttachmentFetchWorkflow other;
  other.SetProfileDataDir(dir_.string());
  other.SetProfileId("test");
  AttachmentLimits roomy;
  roomy.auto_download_max_count_per_minute = 2;
  other.SetLimits(roomy);
  other.SetClockForTest([]() { return int64_t{5'000}; });
  EXPECT_TRUE(other.TryAcceptDirectPush("alice", 100));
  EXPECT_TRUE(other.TryAcceptDirectPush("alice", 100));
  EXPECT_FALSE(other.TryAcceptDirectPush("alice", 100)) << "a third push in the minute";
  EXPECT_TRUE(other.TryAcceptDirectPush("bob", 100)) << "another sender";
  EXPECT_TRUE(other.TryAutoDownloadBudget("alice", 100)) << "pushes are counted apart from downloads";
}

TEST(AttachmentInlineViewGateTest, VideoAboveTheReceiveLimitIsNotShownInline) {
  EXPECT_TRUE(AttachmentAllowsInlinePrivateView("image/png", 50'000'000, 4'000'000));
  EXPECT_TRUE(AttachmentAllowsInlinePrivateView("video/mp4", 0, 4'000'000));
  EXPECT_TRUE(AttachmentAllowsInlinePrivateView("video/mp4", 4'000'000, 4'000'000));
  EXPECT_FALSE(AttachmentAllowsInlinePrivateView("video/mp4", 4'000'001, 4'000'000));
  EXPECT_TRUE(AttachmentAllowsInlinePrivateView("video/mp4", 100'000'000, 100'000'000));
}

} // namespace
} // namespace pbr
