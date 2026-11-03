#include "lib/content/testing/InMemoryObjectIndex.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "common/chat/MessagingLimits.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/delivery/ChatPayloadCodec.h"
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
    workflow_.SetContent(MakeInMemoryIndexedCasStore(dir_.string(), "test"));
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  static std::vector<uint8_t> Hash(const uint8_t seed) { return std::vector<uint8_t>(kAttachmentContentHashSize, seed); }

  static ChatAttachmentFields Fields(const std::string& id, const uint8_t seed, const uint64_t byte_length) {
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/blob/" + id; // fetchable, so only the limits decide
    fields.mime = "application/pdf";
    fields.filename = id + ".pdf";
    fields.byte_length = byte_length;
    fields.content_hash = Hash(seed);
    fields.content_key.assign(kSessionKeySize, 1);
    fields.blob_nonce.assign(kAeadNonceSize, 2);
    return fields;
  }

  static ThreadMessage AttachmentMessage(const std::string& id, const std::string& sender, const uint8_t seed,
                                         const uint64_t byte_length) {
    return MessageWith(id, sender, Fields(id, seed, byte_length));
  }

  static ThreadMessage MessageWith(const std::string& id, const std::string& sender,
                                   const ChatAttachmentFields& fields) {
    ThreadMessage message;
    message.id = id;
    message.thread_id = "t1";
    message.sender_contact_id = sender;
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return message;
  }

  AttachmentFetchWorkflow::DownloadState State(const uint8_t seed, const uint64_t byte_length) const {
    return workflow_.StateFor("t1", Fields("any", seed, byte_length));
  }

  std::filesystem::path dir_;
  AttachmentFetchWorkflow workflow_;
};

TEST_F(ReceiveLimitsTest, DeclaredSizeAboveTheReceiveLimitIsTooLargeAndNeverQueued) {
  AttachmentLimits limits;
  limits.max_receive_bytes = 1000;
  workflow_.SetLimits(limits);
  EXPECT_NE(State(1, 1000), AttachmentFetchWorkflow::DownloadState::TooLarge);
  EXPECT_EQ(State(1, 1001), AttachmentFetchWorkflow::DownloadState::TooLarge);

  // Even a tap on "Download" (force) does not start it.
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("m1", "alice", 1, 5000), /*force=*/true);
  EXPECT_EQ(State(1, 5000), AttachmentFetchWorkflow::DownloadState::TooLarge);
}

TEST_F(ReceiveLimitsTest, ShippingReceiveLimitIsTheDefault) {
  const AttachmentLimits limits = workflow_.Limits();
  EXPECT_EQ(limits.max_receive_bytes, kDefaultAttachmentReceiveMaxBytes);
  EXPECT_EQ(State(2, kDefaultAttachmentReceiveMaxBytes + 1),
            AttachmentFetchWorkflow::DownloadState::TooLarge);
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

TEST_F(ReceiveLimitsTest, LargeFilesStopAt1000MbAndSmallOnesAt1500Mb) {
  // The shipping caps: a store filled by large files still takes the small ones.
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  uint64_t disk = 999'999'999;
  workflow_.SetDiskUsageProvider([&disk]() { return disk; });
  const uint64_t large = kMaxDirectAttachmentBytes + 1;
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 100));
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("bob", large)) << "below 1000 MB";
  disk = 1'000'000'000;
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("carol", large)) << "a large one stops at 1000 MB";
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("carol", kMaxDirectAttachmentBytes)) << "a small one does not";
  disk = 1'499'999'999;
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("dave", 100));
  disk = 1'500'000'000;
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("erin", 100)) << "1500 MB stops the small ones too";
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("frank", large));
}

TEST_F(ReceiveLimitsTest, DownloadEverythingOnAnAllowedNetworkLets80MbAMinuteThrough) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 50'000'000)) << "50 <= 80";
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("alice", 50'000'000)) << "50 + 50 > 80 in the same minute";
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 30'000'000)) << "50 + 30 = 80 is still within";
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("bob", 50'000'000)) << "another sender";
}

TEST_F(ReceiveLimitsTest, OnADisallowedNetworkTheMinuteStaysAt20Mb) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(false);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  int admitted = 0;
  for (int i = 0; i < 10; ++i) {
    const uint8_t seed = static_cast<uint8_t>(40 + i);
    workflow_.EnqueueFromMessage("t1", AttachmentMessage("p" + std::to_string(i), "alice", seed, 3'900'000));
    if (State(seed, 3'900'000) == AttachmentFetchWorkflow::DownloadState::Downloading) {
      ++admitted;
    }
  }
  EXPECT_EQ(admitted, 5) << "5 x 3.9 MB = 19.5 MB; a sixth would pass 20 MB";
  EXPECT_EQ(State(45, 3'900'000), AttachmentFetchWorkflow::DownloadState::Pending);
}

TEST_F(ReceiveLimitsTest, TheEleventhSmallFileInAMinuteIsHeldBackWhateverThePolicy) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  for (int i = 0; i < 11; ++i) {
    workflow_.EnqueueFromMessage("t1", AttachmentMessage("s" + std::to_string(i), "alice",
                                                         static_cast<uint8_t>(60 + i), 1000));
  }
  EXPECT_EQ(State(69, 1000), AttachmentFetchWorkflow::DownloadState::Downloading) << "the tenth";
  EXPECT_EQ(State(70, 1000), AttachmentFetchWorkflow::DownloadState::Pending) << "the eleventh";
}

TEST_F(ReceiveLimitsTest, ThrottledAttachmentShowsDownloadAndNeverStartsByItself) {
  AttachmentLimits limits;
  limits.auto_download_max_count_per_minute = 0; // nothing may start by itself
  workflow_.SetLimits(limits);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });

  workflow_.EnqueueFromMessage("t1", AttachmentMessage("m2", "alice", 3, 2000));
  EXPECT_EQ(State(3, 2000), AttachmentFetchWorkflow::DownloadState::Pending);
  // Looking again (reopening the thread) does not try again either: still a row with "Download".
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("m2", "alice", 3, 2000));
  EXPECT_EQ(State(3, 2000), AttachmentFetchWorkflow::DownloadState::Pending);
}

TEST_F(ReceiveLimitsTest, FullDiskStopsAutomaticDownloadsToo) {
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{2'000'000'000}; });
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("m3", "bob", 4, 2000));
  EXPECT_EQ(State(4, 2000), AttachmentFetchWorkflow::DownloadState::Pending);
}

TEST_F(ReceiveLimitsTest, SmartPolicyStillOnlyStartsSmallFilesByItself) {
  AttachmentLimits limits;
  limits.auto_download_max_bytes = 1000;
  limits.max_receive_bytes = 10'000;
  workflow_.SetLimits(limits);
  EXPECT_EQ(State(5, 5000), AttachmentFetchWorkflow::DownloadState::Pending)
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
  EXPECT_EQ(State(6, 0), AttachmentFetchWorkflow::DownloadState::Pending);
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
  EXPECT_EQ(State(7, 2000), AttachmentFetchWorkflow::DownloadState::Downloading);
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
    (void)State(8, 100);
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
  limits.auto_download_small_disk_cap_bytes = 6000;
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
  other.SetContent(MakeInMemoryIndexedCasStore(dir_.string(), "test"));
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

TEST_F(ReceiveLimitsTest, SmartNeverStartsAFileAboveTheDirectSizeEvenWithATierThatWouldAllowIt) {
  AttachmentLimits limits;
  limits.max_receive_bytes = 100'000'000;
  limits.auto_download_max_bytes = 100'000'000;  // a tier that would allow it
  workflow_.SetLimits(limits);
  workflow_.SetAutoAllowedOnNetwork(true);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  const uint64_t above = kMaxDirectAttachmentBytes + 1;

  EXPECT_EQ(State(10, above), AttachmentFetchWorkflow::DownloadState::Pending);
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("big", "alice", 10, above));
  EXPECT_EQ(State(10, above), AttachmentFetchWorkflow::DownloadState::Pending);
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 100)) << "and it cost no budget";
  // At the direct size it still starts by itself.
  EXPECT_EQ(State(11, kMaxDirectAttachmentBytes), AttachmentFetchWorkflow::DownloadState::Downloading);
}

TEST_F(ReceiveLimitsTest, DownloadEverythingOnAnAllowedNetworkStartsALargeFileWithAServerLink) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  const uint64_t large = 30'000'000;

  EXPECT_EQ(State(10, large), AttachmentFetchWorkflow::DownloadState::Downloading) << "it would start";
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("big", "alice", 10, large));
  EXPECT_EQ(State(10, large), AttachmentFetchWorkflow::DownloadState::Downloading) << "queued";
  EXPECT_FALSE(workflow_.TryAutoDownloadBudget("alice", 60'000'000)) << "its 30 MB were charged (30 + 60 > 80)";
  EXPECT_EQ(State(11, kDefaultAttachmentReceiveMaxBytes), AttachmentFetchWorkflow::DownloadState::Downloading)
      << "up to the receive limit";
  EXPECT_EQ(State(12, kDefaultAttachmentReceiveMaxBytes + 1), AttachmentFetchWorkflow::DownloadState::TooLarge);
}

TEST_F(ReceiveLimitsTest, DownloadEverythingLeavesALargeFileWithoutAServerLinkToATap) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  ChatAttachmentFields fields = Fields("peer", 13, 30'000'000);
  fields.url.clear();  // peer-direct only: the peer refuses a file this large
  EXPECT_EQ(workflow_.StateFor("t1", fields), AttachmentFetchWorkflow::DownloadState::Pending);
  workflow_.EnqueueFromMessage("t1", MessageWith("peer", "alice", fields));
  EXPECT_EQ(workflow_.StateFor("t1", fields), AttachmentFetchWorkflow::DownloadState::Pending);
}

TEST_F(ReceiveLimitsTest, DownloadEverythingOnADisallowedNetworkLeavesALargeFileButTakesASmallOne) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(false);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("big", "alice", 14, 30'000'000));
  EXPECT_EQ(State(14, 30'000'000), AttachmentFetchWorkflow::DownloadState::Pending);
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 20'000'000)) << "it cost no budget";
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("small", "bob", 15, 3'000'000));
  EXPECT_EQ(State(15, 3'000'000), AttachmentFetchWorkflow::DownloadState::Downloading) << "as Smart";
}

TEST_F(ReceiveLimitsTest, DownloadEverythingBeforeAnyNetworkIsKnownKeepsALargeFileQueuedUnjudged) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  EXPECT_EQ(State(16, 30'000'000), AttachmentFetchWorkflow::DownloadState::Downloading) << "not a Download row";
  workflow_.EnqueueFromMessage("t1", AttachmentMessage("big", "alice", 16, 30'000'000));
  EXPECT_EQ(State(16, 30'000'000), AttachmentFetchWorkflow::DownloadState::Downloading);
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 20'000'000)) << "charged only once it is judged";
}

TEST_F(ReceiveLimitsTest, DownloadNothingLeavesAPictureButTakesAVoiceMessage) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::OnDemand);
  workflow_.SetAutoAllowedOnNetwork(true);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });

  ChatAttachmentFields picture = Fields("pic", 20, 3'000'000);
  picture.mime = "image/jpeg";
  picture.filename = "pic.jpg";
  workflow_.EnqueueFromMessage("t1", MessageWith("pic", "alice", picture));
  EXPECT_EQ(workflow_.StateFor("t1", picture), AttachmentFetchWorkflow::DownloadState::Pending);

  ChatAttachmentFields voice = Fields("voice", 21, 25'000);
  voice.mime = "audio/ogg";
  voice.filename = "voice-4200.ogg";
  EXPECT_EQ(workflow_.StateFor("t1", voice), AttachmentFetchWorkflow::DownloadState::Downloading);
  workflow_.EnqueueFromMessage("t1", MessageWith("voice", "alice", voice));
  EXPECT_EQ(workflow_.StateFor("t1", voice), AttachmentFetchWorkflow::DownloadState::Downloading);

  ChatAttachmentFields song = Fields("song", 22, 25'000);
  song.mime = "audio/ogg";
  song.filename = "song.ogg";
  workflow_.EnqueueFromMessage("t1", MessageWith("song", "alice", song));
  EXPECT_EQ(workflow_.StateFor("t1", song), AttachmentFetchWorkflow::DownloadState::Pending);
}

TEST_F(ReceiveLimitsTest, AVoiceMessageUnderDownloadNothingStillCountsAgainstTheMinute) {
  AttachmentLimits limits;
  limits.auto_download_max_count_per_minute = 1;
  workflow_.SetLimits(limits);
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::OnDemand);
  workflow_.SetClockForTest([]() { return int64_t{5'000}; });
  workflow_.SetDiskUsageProvider([]() { return uint64_t{0}; });
  for (int i = 0; i < 2; ++i) {
    ChatAttachmentFields voice = Fields("v" + std::to_string(i), static_cast<uint8_t>(23 + i), 25'000);
    voice.mime = "audio/ogg";
    voice.filename = "voice-" + std::to_string(1000 + i) + ".ogg";
    workflow_.EnqueueFromMessage("t1", MessageWith("v" + std::to_string(i), "alice", voice));
    EXPECT_EQ(workflow_.StateFor("t1", voice), i == 0 ? AttachmentFetchWorkflow::DownloadState::Downloading
                                                       : AttachmentFetchWorkflow::DownloadState::Pending);
  }
}

TEST_F(ReceiveLimitsTest, ProgressIsPerDownloadAndClearsWhenDone) {
  EXPECT_EQ(workflow_.ProgressPercent("t1", Hash(12)), -1);
  workflow_.SetProgress(workflow_.JobKeyFor("t1", Hash(12)), 37);
  EXPECT_EQ(workflow_.ProgressPercent("t1", Hash(12)), 37);
  EXPECT_EQ(workflow_.ProgressPercent("t1", Hash(13)), -1);
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
