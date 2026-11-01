#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/attachments/LargeTransferLane.h"

#include "common/chat/MessagingLimits.h"
#include "common/tests/LocalHttpServer.h"
#include "common/Utilities.h"
#include "foundation/crypto/CryptoConstants.h"
#include "common/runtime/AppRuntime.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatAttachmentPrepare.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "domain/attachments/AttachmentContent.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace pbr {
namespace {

using Direction = LargeTransferLane::Direction;

bool WaitFor(const std::function<bool()>& done, const std::chrono::milliseconds budget = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

TEST(LargeTransferLaneTest, JobsOfOneDirectionRunOneAtATimeInOrder) {
  LargeTransferLane lane;
  std::atomic<int> running{0};
  std::atomic<int> max_running{0};
  std::vector<int> order;
  std::mutex order_mutex;
  std::atomic<int> done{0};
  for (int i = 0; i < 4; ++i) {
    lane.Post(Direction::Download, [&, i]() {
      const int now = ++running;
      max_running = std::max(max_running.load(), now);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      {
        std::lock_guard lock(order_mutex);
        order.push_back(i);
      }
      --running;
      ++done;
    });
  }
  ASSERT_TRUE(WaitFor([&] { return done.load() == 4; }));
  EXPECT_EQ(max_running.load(), 1) << "at most one at a time per direction";
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
}

TEST(LargeTransferLaneTest, UploadsAndDownloadsRunSideBySide) {
  LargeTransferLane lane;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> upload_started{false};
  std::atomic<bool> download_ran{false};
  lane.Post(Direction::Upload, [&upload_started, released]() {
    upload_started = true;
    released.wait();  // a long upload
  });
  ASSERT_TRUE(WaitFor([&] { return upload_started.load(); }));
  lane.Post(Direction::Download, [&]() { download_ran = true; });
  EXPECT_TRUE(WaitFor([&] { return download_ran.load(); })) << "a download is not held by the upload";
  release.set_value();
}

TEST(LargeTransferLaneTest, ALongJobDoesNotHoldTheSharedWorkers) {
  AppRuntime::Initialize();
  {
    LargeTransferLane lane;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> started{false};
    lane.Post(Direction::Download, [&started, released]() {
      started = true;
      released.wait();
    });
    ASSERT_TRUE(WaitFor([&] { return started.load(); }));
    std::atomic<bool> small_ran{false};
    AppRuntime::PostWorkerNormal([&]() { small_ran = true; });
    EXPECT_TRUE(WaitFor([&] { return small_ran.load(); })) << "the small queue's worker is free";
    release.set_value();
  }
  AppRuntime::Shutdown();
}

TEST(LargeTransferLaneTest, ShutdownTellsAJobToStopAndJoins) {
  auto lane = std::make_unique<LargeTransferLane>();
  std::atomic<bool> started{false};
  std::atomic<bool> saw_stop{false};
  lane->Post(Direction::Upload, [&]() {
    started = true;
    while (!lane->Stopping()) {  // what a transfer's progress callback does
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    saw_stop = true;
  });
  ASSERT_TRUE(WaitFor([&] { return started.load(); }));
  lane->Shutdown();  // returns only after the job ended
  EXPECT_TRUE(saw_stop.load());
  std::atomic<bool> late{false};
  lane->Post(Direction::Upload, [&]() { late = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(late.load()) << "nothing runs after the stop";
}

// The workflow with a real runtime, a loopback CDN and the lane.
class LargeDownloadTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    dir_ = std::filesystem::temp_directory_path() / ("pp_large_dl_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    workflow_.SetProfileDataDir(dir_.string());
    workflow_.SetProfileId("test");
    workflow_.SetTransferLane(&lane_);
    workflow_.SetAllowLocalCdnForTest(true);
    ByteVector dek(kDataEncryptionKeySize, 0x31);
    ASSERT_TRUE(workflow_.SetDek(dek));
    AttachmentLimits limits;
    limits.max_receive_bytes = 100'000'000;
    workflow_.SetLimits(limits);
    workflow_.SetDiskUsageProvider([] { return uint64_t{0}; });
  }
  void TearDown() override {
    lane_.Shutdown();
    AppRuntime::Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  ThreadMessage Message(const PreparedChatAttachment& prepared, const std::string& url) {
    ChatAttachmentFields fields = prepared.fields;
    fields.url = url;
    ThreadMessage message;
    message.id = "m1";
    message.thread_id = "t1";
    message.sender_contact_id = "alice";
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return message;
  }

  std::filesystem::path dir_;
  LargeTransferLane lane_;
  AttachmentFetchWorkflow workflow_;
  pbr::test::LocalHttpServer server_;
};

TEST_F(LargeDownloadTest, ALargeFileIsDownloadedOnTheLaneStoredEncryptedAndNotMaterialized) {
  const std::vector<uint8_t> plain(static_cast<size_t>(kMaxDirectAttachmentBytes) + 1000, 0x2c);
  auto prepared = PrepareChatAttachmentFromBytes(plain, "clip.mp4", "", 200'000'000);
  ASSERT_TRUE(prepared);
  server_.SetResponse(std::string(prepared->ciphertext.begin(), prepared->ciphertext.end()));
  const ThreadMessage message = Message(*prepared, server_.Url());

  workflow_.EnqueueFromMessage("t1", message, /*force=*/true);
  ASSERT_TRUE(WaitFor([&] {
    return workflow_.StateFor("t1", prepared->fields.content_hash, prepared->fields.byte_length) ==
           AttachmentFetchWorkflow::DownloadState::Ready;
  }));
  EXPECT_TRUE(AttachmentBlobExists(dir_.string(), prepared->fields.content_hash)) << "stored (encrypted) in the CAS";
  EXPECT_TRUE(AttachmentLocalPath(dir_.string(), "t1", prepared->fields.content_hash, prepared->fields.mime,
                                  prepared->fields.filename)
                  .empty())
      << "no plaintext view until the person opens it";
  EXPECT_FALSE(AttachmentPosterExists(dir_.string(), "t1", prepared->fields.content_hash))
      << "a large video's poster comes with the first Open";
  // Ready is read from the stored file, a moment before the job clears its percent.
  EXPECT_TRUE(WaitFor([&] { return workflow_.ProgressPercent("t1", prepared->fields.content_hash) == -1; }))
      << "no stale percent";
}

TEST_F(LargeDownloadTest, ASmallFileIsStillMaterializedAtOnce) {
  const std::vector<uint8_t> plain(50'000, 0x2d);
  auto prepared = PrepareChatAttachmentFromBytes(plain, "note.pdf", "", 200'000'000);
  ASSERT_TRUE(prepared);
  server_.SetResponse(std::string(prepared->ciphertext.begin(), prepared->ciphertext.end()));
  workflow_.EnqueueFromMessage("t1", Message(*prepared, server_.Url()), /*force=*/true);
  ASSERT_TRUE(WaitFor([&] {
    return workflow_.StateFor("t1", prepared->fields.content_hash, prepared->fields.byte_length) ==
           AttachmentFetchWorkflow::DownloadState::Ready;
  })) << "state "
      << static_cast<int>(workflow_.StateFor("t1", prepared->fields.content_hash, prepared->fields.byte_length));
  // Ready is read from the stored (encrypted) file; the plaintext view is written right after it, so wait for it.
  EXPECT_TRUE(WaitFor([&] {
    return !AttachmentLocalPath(dir_.string(), "t1", prepared->fields.content_hash, prepared->fields.mime,
                                prepared->fields.filename)
                .empty();
  })) << "a small file's plaintext view is there without an Open";
}

// A message deleted while its download is in flight cancels it; the aborted transfer comes back as an error, which
// must not be shown as a failure.
TEST_F(LargeDownloadTest, ACancelledDownloadLeavesNoFailureAndNoPercent) {
  const std::vector<uint8_t> plain(static_cast<size_t>(kMaxDirectAttachmentBytes) + 1000, 0x2e);
  auto prepared = PrepareChatAttachmentFromBytes(plain, "clip.mp4", "", 200'000'000);
  ASSERT_TRUE(prepared);
  server_.SetResponse(std::string(prepared->ciphertext.begin(), prepared->ciphertext.end()));

  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> gate_started{false};
  lane_.Post(Direction::Download, [&gate_started, released]() {  // keeps the job waiting in the lane
    gate_started = true;
    released.wait();
  });
  ASSERT_TRUE(WaitFor([&] { return gate_started.load(); }));

  workflow_.EnqueueFromMessage("t1", Message(*prepared, server_.Url()), /*force=*/true);
  ASSERT_TRUE(WaitFor([&] {
    return workflow_.StateFor("t1", prepared->fields.content_hash, prepared->fields.byte_length) ==
           AttachmentFetchWorkflow::DownloadState::Downloading;
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the drain has handed it to the lane
  workflow_.ForgetThread("t1");                                   // the thread (message) is deleted
  release.set_value();

  std::atomic<bool> lane_idle{false};
  lane_.Post(Direction::Download, [&]() { lane_idle = true; });
  ASSERT_TRUE(WaitFor([&] { return lane_idle.load(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the result is handled on a worker
  EXPECT_NE(workflow_.StateFor("t1", prepared->fields.content_hash, prepared->fields.byte_length),
            AttachmentFetchWorkflow::DownloadState::Failed);
  EXPECT_EQ(workflow_.ProgressPercent("t1", prepared->fields.content_hash), -1);
  EXPECT_FALSE(AttachmentBlobExists(dir_.string(), prepared->fields.content_hash));
}

} // namespace
} // namespace pbr
