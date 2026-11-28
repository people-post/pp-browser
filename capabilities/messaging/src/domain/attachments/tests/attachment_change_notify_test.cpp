// B248: every change notification redraws the whole loaded chat on the UI thread. A batch of downloads asked for one
// per five percent of each and one per state change, none merged. Now at most one waits on the UI thread at a time,
// and progress asks for one at most every 250 ms (state changes are not held back).
#include "lib/content/testing/InMemoryObjectIndex.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"

#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatAttachmentPrepare.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <thread>

namespace pbr {
namespace {

using State = AttachmentFetchWorkflow::DownloadState;

bool WaitFor(const std::function<bool()>& done, const std::chrono::milliseconds budget = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

class ChangeNotifyTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    dir_ = std::filesystem::temp_directory_path() / ("pp_change_notify_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    workflow_.SetProfileDataDir(dir_.string());
    workflow_.SetContent(MakeInMemoryIndexedCasStore(dir_.string(), "test"));
    ASSERT_TRUE(workflow_.SetDek(ByteVector(kDataEncryptionKeySize, 0x41)));
    workflow_.SetDiskUsageProvider([] { return uint64_t{0}; });
    workflow_.SetClockForTest([this]() { return now_ms_.load(); });
    workflow_.SetOnChanged([this]() {
      ++changed_;
      if (on_changed_) {
        on_changed_();
      }
    });
  }
  void TearDown() override {
    AppRuntime::ResumeWorkers();
    AppRuntime::Shutdown();
    // The UI mailbox outlives the runtime and the test: run what this fixture's workflow posted while it is alive.
    for (int i = 0; i < 100 && AppRuntime::HasPendingUITasks(); ++i) {
      AppRuntime::RunUITasks();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /** Runs the UI tasks until none is left (a task may post another). */
  static void RunUI() {
    for (int i = 0; i < 100 && AppRuntime::HasPendingUITasks(); ++i) {
      AppRuntime::RunUITasks();
    }
  }

  /** A small file from alice whose ciphertext already waits on this device (a direct push): no network needed. */
  ThreadMessage PushedFile(const std::string& name, ChatAttachmentFields* fields_out) {
    std::vector<uint8_t> plain(2'000, 0x33);
    plain[0] = static_cast<uint8_t>(name.front());
    auto prepared = PrepareChatAttachmentFromBytes(plain, name + ".bin", "", 1'000'000);
    EXPECT_TRUE(prepared);
    EXPECT_TRUE(
        SavePendingAttachmentCiphertext(dir_.string(), "t1", prepared->fields.content_hash, prepared->ciphertext, {}));
    *fields_out = prepared->fields;
    return MessageWith(name, prepared->fields);
  }
  /** A picture that "download nothing" leaves as a Download row: its enqueue is one state change. */
  static ThreadMessage DownloadRowPicture(const std::string& name, const uint8_t seed) {
    return MessageWith(name, RowFields(name, seed));
  }
  static ChatAttachmentFields RowFields(const std::string& name, const uint8_t seed) {
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/blob/" + name;
    fields.mime = "image/jpeg";
    fields.filename = name + ".jpg";
    fields.byte_length = 3'000;
    fields.content_hash.assign(32, seed);
    fields.content_key.assign(kSessionKeySize, 1);
    fields.blob_nonce.assign(kAeadNonceSize, 2);
    return fields;
  }
  static ThreadMessage MessageWith(const std::string& name, const ChatAttachmentFields& fields) {
    ThreadMessage message;
    message.id = name;
    message.thread_id = "t1";
    message.sender_contact_id = "alice";
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return message;
  }

  std::filesystem::path dir_;
  std::atomic<int64_t> now_ms_{10'000};
  std::atomic<int> changed_{0};
  std::function<void()> on_changed_;
  AttachmentFetchWorkflow workflow_;
};

// One download whose progress steps by 1 % every 10 ms (100 steps, 1 s), the UI running after each step so nothing is
// merged: exactly 4 progress notifications and the one for its end.
//  - Progress: one at least 5 points on from the last one told and 250 ms after it. 5 % (t=50 ms) is the first; then
//    30 % (t=300), 55 % (t=550), 80 % (t=800). 100 % comes at t=1000, 200 ms after the last: held back.
//  - The end (Ready) is a state change: never held back, though it comes right after (and so shows the full bar).
//  - No notification for its start: queued and running both show "Downloading", and the workflow tells neither (so 5,
//    not the plan's "start + done + 4" = 6).
// Before: one per 5 points (20) plus the end = 21.
TEST_F(ChangeNotifyTest, AHundredProgressStepsInOneSecondAreFourNotificationsPlusTheEnd) {
  ChatAttachmentFields fields;
  const ThreadMessage message = PushedFile("doc", &fields);
  AppRuntime::PauseWorkers(); // queued; it runs after the steps
  workflow_.EnqueueFromMessage("t1", message);
  RunUI();
  ASSERT_EQ(changed_.load(), 0) << "a queued download tells nothing";

  const std::string key = workflow_.JobKeyFor("t1", fields.content_hash);
  for (int percent = 1; percent <= 100; ++percent) {
    now_ms_ += 10;
    workflow_.SetProgress(key, percent);
    RunUI();
  }
  EXPECT_EQ(changed_.load(), 4);
  EXPECT_EQ(workflow_.ProgressPercent("t1", fields.content_hash), 100) << "a refresh shows the latest percent";

  AppRuntime::ResumeWorkers();
  // The state turns Ready a moment before its notification is posted: wait for the notification itself.
  ASSERT_TRUE(WaitFor([&] {
    RunUI();
    return changed_.load() >= 5;
  }));
  EXPECT_EQ(workflow_.StateFor("t1", fields), State::Ready);
  AppRuntime::Shutdown(); // nothing else can post
  RunUI();
  EXPECT_EQ(changed_.load(), 5);
}

// A step held back by the 250 ms is not lost: the next step after the gap tells it, though it is not 5 points on from
// the held-back one.
TEST_F(ChangeNotifyTest, AProgressStepHeldBackIsToldByTheNextStepAfterTheGap) {
  const std::string key = workflow_.JobKeyFor("t1", std::vector<uint8_t>(32, 9));
  workflow_.SetProgress(key, 5);
  RunUI();
  EXPECT_EQ(changed_.load(), 1);
  now_ms_ += 100;
  workflow_.SetProgress(key, 10); // 100 ms after the last: held back
  RunUI();
  EXPECT_EQ(changed_.load(), 1);
  now_ms_ += 300;
  workflow_.SetProgress(key, 11); // the download goes on: 6 points on from the last told
  RunUI();
  EXPECT_EQ(changed_.load(), 2);
  EXPECT_EQ(workflow_.ProgressPercent("t1", std::vector<uint8_t>(32, 9)), 11);
}

// Five state changes while the UI has not run: one task waits on the UI thread, and it runs once.
TEST_F(ChangeNotifyTest, ChangesWhileTheUiHasNotRunPostOneTask) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::OnDemand);
  for (int i = 0; i < 5; ++i) {
    workflow_.EnqueueFromMessage("t1", DownloadRowPicture("p" + std::to_string(i), static_cast<uint8_t>(10 + i)));
  }
  AppRuntime::RunUITasks();
  EXPECT_EQ(changed_.load(), 1);
  EXPECT_FALSE(AppRuntime::HasPendingUITasks());
  EXPECT_EQ(workflow_.StateFor("t1", RowFields("p4", 14)), State::Pending) << "the one refresh sees every change";
}

// A change while the UI callback runs (after it began reading) is not lost: one more refresh follows.
TEST_F(ChangeNotifyTest, AChangeWhileTheUiCallbackRunsBringsOneMoreRefresh) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::OnDemand);
  bool once = false;
  on_changed_ = [&]() {
    if (!once) {
      once = true;
      workflow_.EnqueueFromMessage("t1", DownloadRowPicture("late", 20));
    }
  };
  workflow_.EnqueueFromMessage("t1", DownloadRowPicture("first", 21));
  RunUI();
  EXPECT_EQ(changed_.load(), 2);
}

} // namespace
} // namespace pbr
