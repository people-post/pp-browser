// B248: which queued download starts next. A tapped file first; then files from other people, first come first
// served; files the account's other devices sent come after them, but one of theirs after every three of the others'
// so they are never starved, and back to back when nothing else waits. Over the minute's automatic limits, files from
// the account's other devices wait queued and start when the minute allows; others' files still become Download rows.
#include "lib/content/testing/InMemoryObjectIndex.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"

#include "common/Utilities.h"
#include "common/chat/MessagingLimits.h"
#include "common/runtime/AppRuntime.h"
#include "common/thread/ThreadRecordTypes.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/sockets/testing/ScriptedHttpServer.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/ChatAttachmentPrepare.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <thread>

namespace pbr {
namespace {

using State = AttachmentFetchWorkflow::DownloadState;
using pbr::test::ScriptedHttpServer;

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

constexpr const char* kOwn = kLocalSelfContactId;

class DownloadOrderTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    dir_ = std::filesystem::temp_directory_path() / ("pp_download_order_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    workflow_.SetProfileDataDir(dir_.string());
    workflow_.SetContent(MakeInMemoryIndexedCasStore(dir_.string(), "test"));
    workflow_.SetAllowLocalCdnForTest(true);
    ASSERT_TRUE(workflow_.SetDek(ByteVector(kDataEncryptionKeySize, 0x41)));
    workflow_.SetDiskUsageProvider([this] { return disk_bytes_.load(); });
    workflow_.SetClockForTest([this] { return now_ms_.load(); });
    workflow_.SetBudgetWakeForTest([this](const int64_t delay_ms, std::function<void()> wake) {
      std::lock_guard lock(mutex_);
      wakes_.push_back({delay_ms, std::move(wake)});
    });
    server_.SetHandler([this](const ScriptedHttpServer::Request& request) {
      std::lock_guard lock(mutex_);
      return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = bodies_[request.target]};
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

  /** A file in t1 from `sender`, served by the loopback server at `/name`. */
  ThreadMessage File(const std::string& name, const std::string& sender, const size_t bytes = 2'000) {
    std::vector<uint8_t> plain(bytes, static_cast<uint8_t>(name.size()));
    for (size_t i = 0; i < name.size() && i < plain.size(); ++i) {
      plain[i] = static_cast<uint8_t>(name[i]); // a different hash per name
    }
    auto prepared = PrepareChatAttachmentFromBytes(plain, name + ".bin", "", 200'000'000);
    EXPECT_TRUE(prepared);
    ChatAttachmentFields fields = prepared->fields;
    fields.url = server_.Origin() + "/" + name;
    {
      std::lock_guard lock(mutex_);
      bodies_["/" + name] = std::string(prepared->ciphertext.begin(), prepared->ciphertext.end());
      fields_[name] = fields;
    }
    ThreadMessage message;
    message.id = name;
    message.thread_id = "t1";
    message.sender_contact_id = sender;
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return message;
  }
  State StateOf(const std::string& name) {
    ChatAttachmentFields fields;
    {
      std::lock_guard lock(mutex_);
      fields = fields_[name];
    }
    return workflow_.StateFor("t1", fields);
  }
  size_t CountIn(const std::vector<std::string>& names, const State state) {
    size_t count = 0;
    for (const std::string& name : names) {
      count += StateOf(name) == state ? 1 : 0;
    }
    return count;
  }
  /** The files in the order their downloads started (downloads run one at a time). */
  std::vector<std::string> StartOrder() const {
    std::vector<std::string> order;
    for (const auto& request : server_.Requests()) {
      order.push_back(request.target.substr(1));
    }
    return order;
  }
  bool AllReady(const std::vector<std::string>& names) {
    return WaitFor([&] { return CountIn(names, State::Ready) == names.size(); });
  }
  size_t Wakes() {
    std::lock_guard lock(mutex_);
    return wakes_.size();
  }
  /** Runs the oldest budget wake the workflow set. */
  void RunWake() {
    std::function<void()> wake;
    {
      std::lock_guard lock(mutex_);
      ASSERT_FALSE(wakes_.empty());
      EXPECT_EQ(wakes_.front().first, 60'000) << "set for when the minute's window has moved on";
      wake = std::move(wakes_.front().second);
      wakes_.erase(wakes_.begin());
    }
    wake();
  }

  std::mutex mutex_;
  std::map<std::string, std::string> bodies_;
  std::map<std::string, ChatAttachmentFields> fields_;
  std::vector<std::pair<int64_t, std::function<void()>>> wakes_;
  ScriptedHttpServer server_; // after what its handler reads: stopped before those go
  std::filesystem::path dir_;
  std::atomic<int64_t> now_ms_{100'000};
  std::atomic<uint64_t> disk_bytes_{0};
  AttachmentFetchWorkflow workflow_;
};

TEST_F(DownloadOrderTest, TappedFirstThenOthersThenTheOwnDevices) {
  AppRuntime::PauseWorkers(); // all queued before the drain picks
  workflow_.EnqueueFromMessage("t1", File("own", kOwn));
  workflow_.EnqueueFromMessage("t1", File("other", "alice"));
  workflow_.EnqueueFromMessage("t1", File("tapped", "bob"), /*force=*/true);
  AppRuntime::ResumeWorkers();
  ASSERT_TRUE(AllReady({"own", "other", "tapped"}));
  EXPECT_EQ(StartOrder(), (std::vector<std::string>{"tapped", "other", "own"}));
}

// Kenneth 2026-11-28 (B248): own devices' files come last but are never starved: one after every three of the others'.
TEST_F(DownloadOrderTest, OneOwnDeviceFileStartsAfterEveryThreeOfTheOthers) {
  AppRuntime::PauseWorkers();
  workflow_.EnqueueFromMessage("t1", File("s1", kOwn));
  workflow_.EnqueueFromMessage("t1", File("s2", kOwn));
  for (int i = 1; i <= 6; ++i) {
    workflow_.EnqueueFromMessage("t1", File("o" + std::to_string(i), i % 2 == 0 ? "alice" : "bob"));
  }
  AppRuntime::ResumeWorkers();
  ASSERT_TRUE(AllReady({"s1", "s2", "o1", "o2", "o3", "o4", "o5", "o6"}));
  EXPECT_EQ(StartOrder(), (std::vector<std::string>{"o1", "o2", "o3", "s1", "o4", "o5", "o6", "s2"}));
}

TEST_F(DownloadOrderTest, WithNothingElseQueuedOwnDeviceFilesRunBackToBackInOrder) {
  AppRuntime::PauseWorkers();
  for (int i = 1; i <= 4; ++i) {
    workflow_.EnqueueFromMessage("t1", File("s" + std::to_string(i), kOwn));
  }
  AppRuntime::ResumeWorkers();
  ASSERT_TRUE(AllReady({"s1", "s2", "s3", "s4"}));
  EXPECT_EQ(StartOrder(), (std::vector<std::string>{"s1", "s2", "s3", "s4"}));
}

TEST_F(DownloadOrderTest, ATapJumpsTheLineAndDoesNotCountTowardsTheFloor) {
  AppRuntime::PauseWorkers();
  workflow_.EnqueueFromMessage("t1", File("o1", "alice"));
  workflow_.EnqueueFromMessage("t1", File("o2", "alice"));
  workflow_.EnqueueFromMessage("t1", File("s1", kOwn));
  workflow_.EnqueueFromMessage("t1", File("o3", "alice"));
  workflow_.EnqueueFromMessage("t1", File("o4", "alice"));
  workflow_.EnqueueFromMessage("t1", File("tap", "alice"), /*force=*/true);
  AppRuntime::ResumeWorkers();
  ASSERT_TRUE(AllReady({"o1", "o2", "s1", "o3", "o4", "tap"}));
  EXPECT_EQ(StartOrder(), (std::vector<std::string>{"tap", "o1", "o2", "o3", "s1", "o4"}));
}

// Thirty files from the account's other devices at once, the default limit of 10 a minute: none becomes a Download row;
// ten start each minute, woken when the window has moved on.
TEST_F(DownloadOrderTest, ThirtyOwnDeviceFilesAllStartAMinuteAtATimeAndNoneIsThrottled) {
  std::vector<std::string> names;
  for (int i = 0; i < 30; ++i) {
    names.push_back("s" + std::to_string(i));
    workflow_.EnqueueFromMessage("t1", File(names.back(), kOwn));
  }
  EXPECT_EQ(CountIn(names, State::Pending), 0u) << "queued, not Download rows";
  for (size_t minute = 1; minute <= 3; ++minute) {
    ASSERT_TRUE(WaitFor([&] { return CountIn(names, State::Ready) == minute * 10; })) << "minute " << minute;
    if (minute < 3) {
      ASSERT_TRUE(WaitFor([&] { return Wakes() == 1u; })) << "a wake is set for the next minute";
      EXPECT_EQ(CountIn(names, State::Downloading), 30 - minute * 10) << "the rest wait queued";
      EXPECT_EQ(CountIn(names, State::Pending), 0u);
      EXPECT_EQ(CountIn(names, State::Failed), 0u);
      now_ms_ += 60'000;
      RunWake();
    }
  }
  EXPECT_EQ(server_.Requests().size(), 30u);
  EXPECT_EQ(Wakes(), 0u) << "nothing waits any more";
}

// The budget wake is a gated one-shot timer: a reset or quit during its minute drops it unseen. The next kick after
// its deadline (a rewire, a network change, a new file) takes it as lost and sets another; one before it does not.
TEST_F(DownloadOrderTest, ALostBudgetWakeIsSetAgainByTheNextKickAfterItsDeadline) {
  std::vector<std::string> names;
  for (int i = 0; i < 30; ++i) {
    names.push_back("s" + std::to_string(i));
    workflow_.EnqueueFromMessage("t1", File(names.back(), kOwn));
  }
  ASSERT_TRUE(WaitFor([&] { return CountIn(names, State::Ready) == 10u; }));
  ASSERT_TRUE(WaitFor([&] { return Wakes() == 1u; })) << "the first wake (never run: lost to a reset)";

  now_ms_ += 30'000;
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::Smart); // a kick before the deadline
  ASSERT_TRUE(AppRuntime::DrainWorkersThenUI(std::chrono::seconds(5)));
  EXPECT_EQ(Wakes(), 1u) << "the wake may still come: none more";
  EXPECT_EQ(CountIn(names, State::Ready), 10u);

  now_ms_ += 32'000; // past the lost wake's deadline: the minute has also moved on
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::Smart); // what a rewire does
  ASSERT_TRUE(WaitFor([&] { return CountIn(names, State::Ready) == 20u; }));
  ASSERT_TRUE(WaitFor([&] { return Wakes() == 2u; })) << "a new wake for the last ten";
  {
    std::lock_guard lock(mutex_);
    wakes_.erase(wakes_.begin()); // the lost one
  }
  now_ms_ += 60'000;
  RunWake();
  ASSERT_TRUE(AllReady(names));
  EXPECT_EQ(CountIn(names, State::Pending), 0u);
}

// The same burst from another person: still ten, the rest Download rows that never start by themselves.
TEST_F(DownloadOrderTest, ThirtyFilesFromAnotherPersonStillThrottleAsBefore) {
  std::vector<std::string> names;
  for (int i = 0; i < 30; ++i) {
    names.push_back("a" + std::to_string(i));
    workflow_.EnqueueFromMessage("t1", File(names.back(), "alice"));
  }
  EXPECT_EQ(CountIn(names, State::Pending), 20u);
  ASSERT_TRUE(WaitFor([&] { return CountIn(names, State::Ready) == 10u; }));
  now_ms_ += 120'000;
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::Smart); // kicks the drain
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(CountIn(names, State::Pending), 20u);
  EXPECT_EQ(server_.Requests().size(), 10u);
  EXPECT_EQ(Wakes(), 0u);
}

TEST_F(DownloadOrderTest, AFullStoreStillStopsOwnDeviceFiles) {
  disk_bytes_ = 2'000'000'000;
  workflow_.EnqueueFromMessage("t1", File("s1", kOwn));
  EXPECT_EQ(StateOf("s1"), State::Pending) << "a Download row, as from anyone";
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(server_.Requests().size(), 0u);
  EXPECT_EQ(Wakes(), 0u) << "a full store does not empty itself in a minute";
}

TEST_F(DownloadOrderTest, TheNetworkStillGatesALargeOwnDeviceFile) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(false); // cellular, not ticked
  const size_t large = static_cast<size_t>(kMaxDirectAttachmentBytes) + 1'000;
  workflow_.EnqueueFromMessage("t1", File("big", kOwn, large));
  EXPECT_EQ(StateOf("big"), State::Pending);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(server_.Requests().size(), 0u);
}

} // namespace
} // namespace pbr
