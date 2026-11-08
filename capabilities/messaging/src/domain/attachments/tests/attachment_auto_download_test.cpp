#include "lib/content/testing/InMemoryObjectIndex.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/attachments/AttachmentContent.h"
#include "domain/attachments/LargeTransferLane.h"

#include "common/Utilities.h"
#include "common/chat/MessagingLimits.h"
#include "common/runtime/AppRuntime.h"
#include "common/tests/ScriptedHttpServer.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentPlatform.h"
#include "lib/messaging/ChatAttachmentPrepare.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
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
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

constexpr size_t kLargePicture = static_cast<size_t>(kMaxDirectAttachmentBytes) + 1'000'000;

// "Download everything" by network, with a real runtime, the large-file lane and a loopback server whose
// path names the file it serves.
class AutoDownloadByNetworkTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    dir_ = std::filesystem::temp_directory_path() / ("pp_auto_dl_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    workflow_.SetProfileDataDir(dir_.string());
    content_ = MakeInMemoryIndexedCasStore(dir_.string(), "test");
    workflow_.SetContent(content_);
    workflow_.SetTransferLane(&lane_);
    workflow_.SetAllowLocalCdnForTest(true);
    ASSERT_TRUE(workflow_.SetDek(ByteVector(kDataEncryptionKeySize, 0x41)));
    AttachmentLimits limits;
    limits.max_receive_bytes = 100'000'000;
    workflow_.SetLimits(limits);
    workflow_.SetDiskUsageProvider([] { return uint64_t{0}; });
    server_.SetHandler([this](const ScriptedHttpServer::Request& request) {
      std::shared_future<void> hold;
      std::string body;
      {
        std::lock_guard lock(mutex_);
        hold = holds_[request.target];
        body = bodies_[request.target];
      }
      if (hold.valid()) {
        hold.wait();
      }
      return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = body};
    });
  }
  void TearDown() override {
    AppRuntime::ResumeWorkers();
    lane_.Shutdown();
    AppRuntime::Shutdown();
    SetAttachmentPlatform({});
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /** The plaintext Message(name, bytes) serves. */
  static std::vector<uint8_t> Plaintext(const std::string& name, const size_t bytes) {
    std::vector<uint8_t> plain(bytes, static_cast<uint8_t>(name.size() + name.back()));
    plain[0] = static_cast<uint8_t>(name.front()); // a different hash per name
    return plain;
  }
  /** A served attachment message from alice: `name` is its path on the server; `mime` what it says it is. */
  ThreadMessage Message(const std::string& name, const size_t bytes, ChatAttachmentFields* fields_out,
                        const std::string& mime = "") {
    auto prepared = PrepareChatAttachmentFromBytes(Plaintext(name, bytes), name + ".bin", "", 200'000'000);
    EXPECT_TRUE(prepared);
    {
      std::lock_guard lock(mutex_);
      bodies_["/" + name] = std::string(prepared->ciphertext.begin(), prepared->ciphertext.end());
    }
    ChatAttachmentFields fields = prepared->fields;
    fields.url = server_.Origin() + "/" + name;
    if (!mime.empty()) {
      fields.mime = mime;
      fields.filename = name + ".png";
    }
    ThreadMessage message;
    message.id = name;
    message.thread_id = "t1";
    message.sender_contact_id = "alice";
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    *fields_out = fields;
    return message;
  }
  /** The server holds its answer for `name` until the returned promise is set. */
  std::promise<void> HoldAnswer(const std::string& name) {
    std::promise<void> release;
    std::lock_guard lock(mutex_);
    holds_["/" + name] = release.get_future().share();
    return release;
  }
  size_t Gets(const std::string& name) const {
    size_t count = 0;
    for (const auto& request : server_.Requests()) {
      count += request.target == "/" + name ? 1 : 0;
    }
    return count;
  }
  /** A large picture already in the store (as if downloaded in an earlier run). */
  void StorePicture(const std::string& name, ChatAttachmentFields* fields) {
    (void)Message(name, kLargePicture, fields, "image/png");
    ASSERT_TRUE(SaveAttachmentPlaintext(*content_, fields->content_hash, fields->mime, Plaintext(name, kLargePicture),
                                        fields->filename, ByteVector(kDataEncryptionKeySize, 0x41)));
    ASSERT_EQ(StateOf(*fields), State::Ready);
  }
  State StateOf(const ChatAttachmentFields& fields) const { return workflow_.StateFor("t1", fields); }
  bool Becomes(const ChatAttachmentFields& fields, const State state) {
    return WaitFor([&] { return StateOf(fields) == state; });
  }

  std::mutex mutex_;
  std::map<std::string, std::string> bodies_;
  std::map<std::string, std::shared_future<void>> holds_;
  ScriptedHttpServer server_;  // after what its handler reads: stopped before those go
  std::filesystem::path dir_;
  std::shared_ptr<CasStore> content_;
  LargeTransferLane lane_;
  AttachmentFetchWorkflow workflow_;
};

constexpr size_t kLarge = static_cast<size_t>(kMaxDirectAttachmentBytes) + 1'000'000; // ~5.2 MB

/** Every file under `root` (none when it does not exist). */
std::vector<std::filesystem::path> FilesUnder(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file()) {
      files.push_back(it->path());
    }
  }
  return files;
}

TEST_F(AutoDownloadByNetworkTest, ThirtyMegabytesComeByThemselvesOnWifiAndAreStored) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  ChatAttachmentFields fields;
  const ThreadMessage message = Message("thirty", 30'000'000, &fields);

  workflow_.EnqueueFromMessage("t1", message);  // what a new message does: no tap
  ASSERT_TRUE(Becomes(fields, State::Ready)) << "state " << static_cast<int>(StateOf(fields));
  EXPECT_TRUE(AttachmentBlobExists(*content_, fields.content_hash)) << "stored (encrypted)";
  EXPECT_EQ(Gets("thirty"), 1u);
}

TEST_F(AutoDownloadByNetworkTest, SmartLeavesTheSameThirtyMegabytesADownloadRow) {
  workflow_.SetAutoAllowedOnNetwork(true);
  ChatAttachmentFields fields;
  const ThreadMessage message = Message("thirty", 30'000'000, &fields);

  workflow_.EnqueueFromMessage("t1", message);
  EXPECT_EQ(StateOf(fields), State::Pending);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(StateOf(fields), State::Pending);
  EXPECT_EQ(Gets("thirty"), 0u);
}

// Allowed when queued, not when it would start: a Download row, its budget given back, nothing fetched.
TEST_F(AutoDownloadByNetworkTest, ANetworkChangeBeforeTheDrainStartsItHoldsTheFileBackAndRefunds) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  ChatAttachmentFields fields;
  const ThreadMessage message = Message("large", kLarge, &fields);

  AppRuntime::PauseWorkers();  // the drain is posted but does not run yet
  workflow_.EnqueueFromMessage("t1", message);
  EXPECT_EQ(StateOf(fields), State::Downloading) << "queued";
  workflow_.SetAutoAllowedOnNetwork(false);  // e.g. Wi-Fi lost, cellular not ticked
  EXPECT_EQ(StateOf(fields), State::Downloading) << "still queued: judged when it starts";
  AppRuntime::ResumeWorkers();

  ASSERT_TRUE(Becomes(fields, State::Pending));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(StateOf(fields), State::Pending);
  EXPECT_EQ(Gets("large"), 0u);
  // 20 MB a minute on this network: had its ~5 MB not been given back, 19 MB more would not fit.
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 19'000'000)) << "its budget was given back";
}

// A large file waits in the lane behind another; the network changes meanwhile: judged again when the lane takes it.
TEST_F(AutoDownloadByNetworkTest, ANetworkChangeWhileItWaitsInTheLaneHoldsItBackAndRefunds) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> gate_started{false};
  lane_.Post(LargeTransferLane::Direction::Download, [&gate_started, released]() {
    gate_started = true;
    released.wait();
  });
  ASSERT_TRUE(WaitFor([&] { return gate_started.load(); }));

  ChatAttachmentFields large;
  ChatAttachmentFields small;
  const ThreadMessage large_message = Message("large", kLarge, &large);
  const ThreadMessage small_message = Message("small", 2'000, &small);
  workflow_.EnqueueFromMessage("t1", large_message);
  workflow_.EnqueueFromMessage("t1", small_message);
  // The drain takes them in order: the small one done means the large one was handed to the lane.
  EXPECT_TRUE(Becomes(small, State::Ready));
  EXPECT_EQ(StateOf(large), State::Downloading);

  workflow_.SetAutoAllowedOnNetwork(false);
  release.set_value();
  ASSERT_TRUE(Becomes(large, State::Pending));
  std::atomic<bool> lane_idle{false};
  lane_.Post(LargeTransferLane::Direction::Download, [&]() { lane_idle = true; });
  ASSERT_TRUE(WaitFor([&] { return lane_idle.load(); }));
  EXPECT_EQ(StateOf(large), State::Pending);
  EXPECT_EQ(Gets("large"), 0u);
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 19'000'000)) << "its budget was given back";
}

TEST_F(AutoDownloadByNetworkTest, ARunningDownloadIsNotStoppedByANetworkChange) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  ChatAttachmentFields fields;
  const ThreadMessage message = Message("large", kLarge, &fields);
  std::promise<void> answer = HoldAnswer("large");

  workflow_.EnqueueFromMessage("t1", message);
  EXPECT_TRUE(WaitFor([&] { return Gets("large") == 1u; })) << "the download has started";
  workflow_.SetAutoAllowedOnNetwork(false);
  answer.set_value();
  ASSERT_TRUE(Becomes(fields, State::Ready));
}

TEST_F(AutoDownloadByNetworkTest, AFileHeldBackForTheNetworkComesByItselfWhenItIsAllowed) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(false);  // on cellular, cellular not ticked
  ChatAttachmentFields fields;
  const ThreadMessage message = Message("large", kLarge, &fields);

  workflow_.EnqueueFromMessage("t1", message);
  EXPECT_EQ(StateOf(fields), State::Pending);
  workflow_.SetAutoAllowedOnNetwork(false);  // another disallowed report changes nothing
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(Gets("large"), 0u);

  workflow_.SetAutoAllowedOnNetwork(true);  // back on Wi-Fi
  ASSERT_TRUE(Becomes(fields, State::Ready));
  EXPECT_EQ(Gets("large"), 1u);
}

TEST_F(AutoDownloadByNetworkTest, OnlyFilesHeldBackForTheNetworkComeBack) {
  AttachmentLimits limits = workflow_.Limits();
  limits.auto_download_all_max_bytes_per_minute = 1'000'000;  // a large file is over the minute at once
  workflow_.SetLimits(limits);
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);

  // Held back by the limits on an allowed network.
  workflow_.SetAutoAllowedOnNetwork(true);
  ChatAttachmentFields throttled;
  workflow_.EnqueueFromMessage("t1", Message("throttled", kLarge, &throttled));
  EXPECT_EQ(StateOf(throttled), State::Pending);

  // Held back for the network, then its thread deleted.
  workflow_.SetAutoAllowedOnNetwork(false);
  ChatAttachmentFields forgotten;
  ThreadMessage forgotten_message = Message("forgotten", kLarge, &forgotten);
  forgotten_message.thread_id = "t2";
  workflow_.EnqueueFromMessage("t2", forgotten_message);
  EXPECT_EQ(workflow_.StateFor("t2", forgotten), State::Pending);
  workflow_.ForgetThread("t2");

  workflow_.SetAutoAllowedOnNetwork(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(StateOf(throttled), State::Pending);
  EXPECT_EQ(Gets("throttled"), 0u);
  EXPECT_EQ(Gets("forgotten"), 0u);
}

TEST_F(AutoDownloadByNetworkTest, BeforeAnyNetworkIsKnownALargeFileWaitsQueuedThenStartsOnWifi) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  ChatAttachmentFields large;
  ChatAttachmentFields small;
  workflow_.EnqueueFromMessage("t1", Message("large", kLarge, &large));
  workflow_.EnqueueFromMessage("t1", Message("small", 2'000, &small));
  ASSERT_TRUE(Becomes(small, State::Ready)) << "a small one does not wait behind it";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(StateOf(large), State::Downloading) << "queued, not a Download row";
  EXPECT_EQ(Gets("large"), 0u);

  workflow_.SetAutoAllowedOnNetwork(true);  // the Wi-Fi baseline arrives
  ASSERT_TRUE(Becomes(large, State::Ready));
}

TEST_F(AutoDownloadByNetworkTest, BeforeAnyNetworkIsKnownALargeFileBecomesADownloadRowOnADisallowedOne) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  ChatAttachmentFields large;
  workflow_.EnqueueFromMessage("t1", Message("large", kLarge, &large));
  EXPECT_EQ(StateOf(large), State::Downloading);

  workflow_.SetAutoAllowedOnNetwork(false);  // the baseline is cellular, not ticked
  ASSERT_TRUE(Becomes(large, State::Pending));
  EXPECT_EQ(Gets("large"), 0u);
  EXPECT_TRUE(workflow_.TryAutoDownloadBudget("alice", 20'000'000)) << "it was never charged";

  workflow_.SetAutoAllowedOnNetwork(true);  // then Wi-Fi
  ASSERT_TRUE(Becomes(large, State::Ready));
}

TEST_F(AutoDownloadByNetworkTest, ATapStartsALargeFileThatWaitsForTheNetworkToBeKnown) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  ChatAttachmentFields large;
  const ThreadMessage message = Message("large", kLarge, &large);
  workflow_.EnqueueFromMessage("t1", message);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(Gets("large"), 0u);

  workflow_.EnqueueFromMessage("t1", message, /*force=*/true);  // the person taps it
  ASSERT_TRUE(Becomes(large, State::Ready));
  EXPECT_EQ(Gets("large"), 1u);
}

// A tap on a large file waiting in the lane behind another: it downloads even though the network does not allow
// "download everything" any more (the lane's second look skips the network for a tapped one).
TEST_F(AutoDownloadByNetworkTest, ATapOnALargeFileWaitingInTheLaneDownloadsItOnADisallowedNetwork) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> gate_started{false};
  lane_.Post(LargeTransferLane::Direction::Download, [&gate_started, released]() {
    gate_started = true;
    released.wait();
  });
  ASSERT_TRUE(WaitFor([&] { return gate_started.load(); }));

  ChatAttachmentFields large;
  ChatAttachmentFields small;
  const ThreadMessage large_message = Message("large", kLarge, &large);
  workflow_.EnqueueFromMessage("t1", large_message);
  workflow_.EnqueueFromMessage("t1", Message("small", 2'000, &small));
  EXPECT_TRUE(Becomes(small, State::Ready)) << "so the large one was handed to the lane";
  EXPECT_EQ(StateOf(large), State::Downloading);

  workflow_.SetAutoAllowedOnNetwork(false);                       // Wi-Fi lost, cellular not ticked
  workflow_.EnqueueFromMessage("t1", large_message, /*force=*/true); // the person taps Download meanwhile
  release.set_value();
  ASSERT_TRUE(Becomes(large, State::Ready)) << "state " << static_cast<int>(StateOf(large));
  EXPECT_EQ(Gets("large"), 1u);
}

// A tap on a large file still in the drain's queue: the drain's look skips the network for it too.
TEST_F(AutoDownloadByNetworkTest, ATapOnAQueuedLargeFileDownloadsItOnADisallowedNetwork) {
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  ChatAttachmentFields large;
  const ThreadMessage message = Message("large", kLarge, &large);

  AppRuntime::PauseWorkers(); // the drain is posted but does not run yet
  workflow_.EnqueueFromMessage("t1", message);
  workflow_.SetAutoAllowedOnNetwork(false);
  workflow_.EnqueueFromMessage("t1", message, /*force=*/true);
  AppRuntime::ResumeWorkers();
  ASSERT_TRUE(Becomes(large, State::Ready)) << "state " << static_cast<int>(StateOf(large));
  EXPECT_EQ(Gets("large"), 1u);
}

// A large picture that comes in gets its thumbnail from the bytes the download holds: no second decrypt, and
// nothing but the thumbnail is written under the view cache.
TEST_F(AutoDownloadByNetworkTest, ADownloadedLargePictureGetsItsThumbnailFromTheBytesInHand) {
  std::atomic<int> calls{0};
  std::atomic<bool> same_bytes{false};
  std::atomic<size_t> files_while_running{99};
  const auto view_root = std::filesystem::path(AttachmentViewRoot(dir_.string(), "t1"));
  const std::vector<uint8_t> plain = Plaintext("pic", kLarge);
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>& bytes,
                                                     int) -> Roe<std::vector<uint8_t>> {
    ++calls;
    same_bytes = bytes == plain;
    files_while_running = FilesUnder(view_root).size();
    return std::vector<uint8_t>{0xff, 0xd8, 0xff, 0xd9};
  }});
  workflow_.SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_.SetAutoAllowedOnNetwork(true);
  ChatAttachmentFields fields;
  workflow_.EnqueueFromMessage("t1", Message("pic", kLarge, &fields, "image/png"));
  ASSERT_TRUE(WaitFor([&] { return AttachmentPosterExists(dir_.string(), "t1", fields.content_hash); }));
  EXPECT_EQ(StateOf(fields), State::Ready);
  EXPECT_EQ(Gets("pic"), 1u);
  EXPECT_EQ(calls.load(), 1);
  EXPECT_TRUE(same_bytes.load());
  EXPECT_EQ(files_while_running.load(), 0u) << "no plaintext file for the hook";
  EXPECT_TRUE(AttachmentPosterExists(dir_.string(), "t1", fields.content_hash));
  const auto files = FilesUnder(view_root);
  ASSERT_EQ(files.size(), 1u) << "only the thumbnail";
  EXPECT_EQ(files[0].string(), AttachmentPosterPath(dir_.string(), "t1", fields.content_hash));
}

// After a restart (the view cache wiped) the transcript asks again: made once on a worker however often it asks,
// from the store into memory, and the change is notified.
TEST_F(AutoDownloadByNetworkTest, AWipedThumbnailIsMadeAgainOnceOnAWorkerAndNotified) {
  ChatAttachmentFields fields;
  StorePicture("pic", &fields); // stored earlier; its thumbnail went with the view cache at the restart
  ASSERT_TRUE(WipeAllAttachmentViewCaches(dir_.string()));
  ASSERT_FALSE(AttachmentPosterExists(dir_.string(), "t1", fields.content_hash));

  std::atomic<int> changed{0};
  workflow_.SetOnChanged([&changed]() { ++changed; });
  std::atomic<int> calls{0};
  std::atomic<bool> same_bytes{false};
  std::atomic<bool> on_caller_thread{true};
  const std::thread::id caller = std::this_thread::get_id();
  std::promise<void> release;
  auto released = release.get_future().share();
  const std::vector<uint8_t> plain = Plaintext("pic", kLarge);
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&, released](const std::vector<uint8_t>& bytes,
                                                               int) -> Roe<std::vector<uint8_t>> {
    ++calls;
    same_bytes = bytes == plain;
    on_caller_thread = std::this_thread::get_id() == caller;
    released.wait();
    return std::vector<uint8_t>{0xff, 0xd8, 0xff, 0xd9};
  }});

  workflow_.RequestImageThumbnail("t1", fields);
  ASSERT_TRUE(WaitFor([&] { return calls.load() == 1; }));
  workflow_.RequestImageThumbnail("t1", fields); // a re-render while it is being made
  workflow_.RequestImageThumbnail("t1", fields);
  release.set_value();
  ASSERT_TRUE(WaitFor([&] {
    AppRuntime::RunUITasks();
    return changed.load() > 0;
  }));
  EXPECT_TRUE(AttachmentPosterExists(dir_.string(), "t1", fields.content_hash));
  EXPECT_EQ(calls.load(), 1) << "one build per attachment";
  EXPECT_TRUE(same_bytes.load());
  EXPECT_FALSE(on_caller_thread.load()) << "never on the render thread";
  EXPECT_TRUE(FilesUnder(std::filesystem::path(AttachmentViewRoot(dir_.string(), "t1"))).size() == 1u);
}

TEST_F(AutoDownloadByNetworkTest, AThumbnailThatCannotBeMadeIsNotTriedAgainThisSession) {
  ChatAttachmentFields fields;
  StorePicture("pic", &fields);

  std::atomic<int> calls{0};
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
    ++calls;
    return Error("too many pixels");
  }});
  workflow_.RequestImageThumbnail("t1", fields);
  ASSERT_TRUE(WaitFor([&] { return calls.load() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  workflow_.RequestImageThumbnail("t1", fields);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(calls.load(), 1);
  EXPECT_FALSE(AttachmentPosterExists(dir_.string(), "t1", fields.content_hash));
}

// Kenneth 2026-11-06: a sound file's music card reads the file's own tags once, on a worker, and draws them after the
// change notification; a lock forgets them with the view cache (they were read from plaintext).
TEST_F(AutoDownloadByNetworkTest, ASoundFilesTagsAreReadOnceOnAWorkerAndForgottenAtALock) {
  const std::vector<uint8_t> hash(32, 7);
  const std::filesystem::path view = dir_ / "song.mp3";
  {
    std::ofstream out(view, std::ios::binary);
    out << "not really a sound file";
  }
  std::atomic<int> changed{0};
  workflow_.SetOnChanged([&changed]() { ++changed; });
  EXPECT_FALSE(workflow_.AudioTags("t1", hash).has_value()) << "nothing read yet";
  workflow_.RequestAudioTags("t1", hash, view.string());
  workflow_.RequestAudioTags("t1", hash, view.string()); // a re-render while it is being read
  ASSERT_TRUE(WaitFor([&] {
    AppRuntime::RunUITasks();
    return changed.load() > 0 && workflow_.AudioTags("t1", hash).has_value();
  }));
  const auto tags = workflow_.AudioTags("t1", hash);
  EXPECT_TRUE(tags->title.empty()) << "the file names no title: the card keeps the file name";
  EXPECT_FALSE(AttachmentPosterExists(dir_.string(), "t1", hash)) << "no picture in it: no cover";
  EXPECT_FALSE(workflow_.AudioTags("t2", hash).has_value()) << "per chat";
  workflow_.ClearDek();
  EXPECT_FALSE(workflow_.AudioTags("t1", hash).has_value());
}

} // namespace
} // namespace pbr
