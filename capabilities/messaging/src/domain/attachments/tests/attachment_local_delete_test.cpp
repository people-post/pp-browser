#include "lib/content/testing/InMemoryObjectIndex.h"
#include "domain/attachments/AttachmentContent.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"

#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"
#include "common/tests/ScriptedHttpServer.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/AiImageRef.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentPlatform.h"
#include "lib/messaging/AttachmentSuppressionStore.h"
#include "lib/messaging/ChatAttachmentPrepare.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

// asio (via ScriptedHttpServer.h) pulls in <windows.h>, whose GetMessage macro would rewrite
// IThreadTranscript::GetMessage below to GetMessageA on MSVC (C2039). We call the method, not the Win32 pump.
#ifdef GetMessage
#undef GetMessage
#endif

namespace pbr {
namespace {

using State = AttachmentFetchWorkflow::DownloadState;
using pbr::test::ScriptedHttpServer;

bool WaitFor(const std::function<bool()>& done, const std::chrono::milliseconds budget = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

ByteVector Dek() { return ByteVector(kDataEncryptionKeySize, 0x5a); }

/** Me → Storage's local delete, its lists and the storage grid's thumbnails, on a real runtime and a loopback CDN. */
class AttachmentLocalDeleteTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    dir_ = std::filesystem::temp_directory_path() / ("pp_local_delete_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    content_ = MakeInMemoryIndexedCasStore(dir_.string(), "test");
    suppression_ = std::make_unique<AttachmentSuppressionStore>(dir_.string());
    workflow_ = std::make_unique<AttachmentFetchWorkflow>();
    Wire(*workflow_, *suppression_);
    for (const auto& [id, kind] : std::vector<std::pair<std::string, ThreadKind>>{
             {"A", ThreadKind::Direct}, {"B", ThreadKind::Direct}, {"C", ThreadKind::Group}, {"ai", ThreadKind::Ai}}) {
      Thread thread;
      thread.id = id;
      thread.kind = kind;
      thread.title = id;
      ASSERT_TRUE(threads_.UpsertThread(thread));
    }
    server_.SetHandler([this](const ScriptedHttpServer::Request& request) {
      std::lock_guard lock(mutex_);
      const auto found = bodies_.find(request.target);
      if (found == bodies_.end()) {
        return ScriptedHttpServer::Reply{.status = 404, .headers = {}, .body = "{}"};
      }
      return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = found->second};
    });
  }
  void TearDown() override {
    AppRuntime::Shutdown();
    workflow_.reset();
    SetAttachmentPlatform({});
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  void Wire(AttachmentFetchWorkflow& workflow, AttachmentSuppressionStore& suppression) {
    workflow.SetProfileDataDir(dir_.string());
    workflow.SetContent(content_);
    workflow.SetSuppressionStore(&suppression);
    workflow.SetAllowLocalCdnForTest(true);
    ASSERT_TRUE(workflow.SetDek(Dek()));
    workflow.SetDiskUsageProvider([] { return uint64_t{0}; });
    AttachmentLimits limits;
    limits.max_receive_bytes = 100'000'000;
    workflow.SetLimits(limits);
  }

  struct File {
    ChatAttachmentFields fields;
    std::vector<uint8_t> plain;
  };
  /** A file served at /<name> (its ciphertext), typed `mime` when given. */
  File MakeFile(const std::string& name, const size_t bytes, const std::string& mime = "image/png") {
    std::vector<uint8_t> plain(bytes, static_cast<uint8_t>(name.size()));
    for (size_t i = 0; i < name.size() && i < plain.size(); ++i) {
      plain[i] = static_cast<uint8_t>(name[i]); // a different hash per name
    }
    auto prepared = PrepareChatAttachmentFromBytes(plain, name + ".bin", "", 200'000'000);
    EXPECT_TRUE(prepared);
    File file{.fields = prepared->fields, .plain = std::move(plain)};
    {
      std::lock_guard lock(mutex_);
      bodies_["/" + name] = std::string(prepared->ciphertext.begin(), prepared->ciphertext.end());
    }
    file.fields.url = server_.Origin() + "/" + name;
    file.fields.mime = mime;
    file.fields.filename = name + (IsAttachmentVideoMime(mime) ? ".mp4" : ".png");
    return file;
  }
  /** The file's bytes on this device, as a download or a send leaves them. */
  void Keep(const File& file) {
    ASSERT_TRUE(SaveAttachmentPlaintext(*content_, file.fields.content_hash, file.fields.mime, file.plain,
                                        file.fields.filename, Dek()));
  }
  ThreadMessage Message(const std::string& thread_id, const std::string& id, const ChatAttachmentFields& fields,
                        const std::string& sender = "alice") {
    ThreadMessage message;
    message.id = id;
    message.thread_id = thread_id;
    message.sender_contact_id = sender;
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    message.timestamp = 1000;
    return message;
  }
  ThreadMessage Post(const std::string& thread_id, const std::string& id, const ChatAttachmentFields& fields,
                     const std::string& sender = "alice") {
    ThreadMessage message = Message(thread_id, id, fields, sender);
    EXPECT_TRUE(threads_.AppendMessage(message));
    return message;
  }
  void PostAiImage(const std::string& id, const ChatAttachmentFields& fields) {
    ThreadMessage message;
    message.id = id;
    message.thread_id = "ai";
    message.sender_contact_id = "me";
    message.content_type = ChatContentType::Text;
    message.text = "[Image] what is this?";
    message.ai_image_json = AiImageRefToJson(AiImageRef{.content_hash = fields.content_hash, .mime = "image/png"});
    ASSERT_TRUE(threads_.AppendMessage(message));
  }
  bool Stored(const ChatAttachmentFields& fields) const { return AttachmentBlobExists(*content_, fields.content_hash); }
  State StateIn(const std::string& thread_id, const ChatAttachmentFields& fields) const {
    return workflow_->StateFor(thread_id, fields);
  }
  StoredAttachmentDeleteResult Delete(std::vector<StoredAttachmentRef> items) {
    StoredAttachmentDeleteResult result;
    bool called = false;
    workflow_->DeleteLocalAttachments(threads_, std::move(items), [&](StoredAttachmentDeleteResult r) {
      result = r;
      called = true;
    });
    EXPECT_TRUE(called);
    return result;
  }
  /** Files of this object in the thread's view folder (view, poster, thumbnail). */
  size_t ViewFiles(const std::string& thread_id, const ChatAttachmentFields& fields) const {
    std::error_code ec;
    size_t count = 0;
    const std::string hex = AttachmentHashHex(fields.content_hash);
    for (std::filesystem::directory_iterator it(AttachmentViewRoot(dir_.string(), thread_id), ec), end;
         !ec && it != end; it.increment(ec)) {
      count += it->path().filename().string().rfind(hex, 0) == 0 ? 1 : 0;
    }
    return count;
  }
  size_t Gets(const std::string& name) const {
    size_t count = 0;
    for (const auto& request : server_.Requests()) {
      count += request.target == "/" + name ? 1 : 0;
    }
    return count;
  }
  std::vector<StoredAttachmentItem> ItemsIn(const std::string& thread_id) {
    std::vector<StoredAttachmentItem> mine;
    for (StoredAttachmentItem& item : workflow_->ListStoredAttachments(threads_)) {
      if (item.thread_id == thread_id) {
        mine.push_back(std::move(item));
      }
    }
    return mine;
  }

  std::filesystem::path dir_;
  std::shared_ptr<CasStore> content_;
  std::unique_ptr<AttachmentSuppressionStore> suppression_;
  std::unique_ptr<AttachmentFetchWorkflow> workflow_;
  InMemoryDeliveryStore threads_;
  ScriptedHttpServer server_;
  std::mutex mutex_;
  std::map<std::string, std::string> bodies_;
};

TEST_F(AttachmentLocalDeleteTest, ADeletedItemShowsDownloadAndItsBytesAreGone) {
  const File photo = MakeFile("photo", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  ASSERT_TRUE(workflow_->EnsureLocalViewPath("A", photo.fields.content_hash, photo.fields.mime,
                                             photo.fields.filename));
  ASSERT_EQ(StateIn("A", photo.fields), State::Ready);
  ASSERT_EQ(ItemsIn("A").size(), 1u);
  const uint64_t stored = AttachmentLocalBytes(*content_, photo.fields.content_hash);
  EXPECT_EQ(workflow_->ReleasableBytes(threads_, {{"A", photo.fields.content_hash}}), stored);

  const auto result = Delete({{"A", photo.fields.content_hash}});

  EXPECT_EQ(result.deleted, 1u);
  EXPECT_EQ(result.kept_shared, 0u);
  EXPECT_EQ(result.freed_bytes, stored);
  EXPECT_FALSE(Stored(photo.fields));
  EXPECT_EQ(StateIn("A", photo.fields), State::Pending);
  EXPECT_EQ(ViewFiles("A", photo.fields), 0u);
  EXPECT_TRUE(ItemsIn("A").empty());
  EXPECT_TRUE(suppression_->IsSuppressed("A", photo.fields.content_hash));
}

TEST_F(AttachmentLocalDeleteTest, BytesAnotherChatUsesStayAndShowThereButNotHere) {
  const File photo = MakeFile("shared", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  Post("B", "b1", photo.fields);
  ASSERT_TRUE(workflow_->EnsureLocalViewPath("A", photo.fields.content_hash, photo.fields.mime,
                                             photo.fields.filename));
  EXPECT_EQ(workflow_->ReleasableBytes(threads_, {{"A", photo.fields.content_hash}}), 0u);

  const auto result = Delete({{"A", photo.fields.content_hash}});

  EXPECT_EQ(result.deleted, 1u);
  EXPECT_EQ(result.kept_shared, 1u);
  EXPECT_EQ(result.freed_bytes, 0u);
  EXPECT_TRUE(Stored(photo.fields));
  EXPECT_EQ(StateIn("A", photo.fields), State::Pending) << "deleted in this chat: Download here";
  EXPECT_EQ(StateIn("B", photo.fields), State::Ready) << "the other chat still shows it";
  EXPECT_EQ(ViewFiles("A", photo.fields), 0u);
  EXPECT_FALSE(workflow_->EnsureLocalViewPath("A", photo.fields.content_hash, photo.fields.mime,
                                              photo.fields.filename));
  EXPECT_TRUE(workflow_->EnsureLocalViewPath("B", photo.fields.content_hash, photo.fields.mime,
                                             photo.fields.filename));
  EXPECT_TRUE(ItemsIn("A").empty()) << "not in this chat's list";
  EXPECT_EQ(ItemsIn("B").size(), 1u);
}

TEST_F(AttachmentLocalDeleteTest, AnAiImageUsingTheBytesKeepsThem) {
  const File photo = MakeFile("asked", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  PostAiImage("q1", photo.fields);
  EXPECT_EQ(workflow_->ReleasableBytes(threads_, {{"A", photo.fields.content_hash}}), 0u);

  const auto result = Delete({{"A", photo.fields.content_hash}});

  EXPECT_EQ(result.kept_shared, 1u);
  EXPECT_TRUE(Stored(photo.fields));
  EXPECT_EQ(StateIn("A", photo.fields), State::Pending);
  EXPECT_TRUE(workflow_->EnsureLocalViewPath("ai", photo.fields.content_hash, "image/png", ""));
}

TEST_F(AttachmentLocalDeleteTest, DeletingFromTheLargeFilesListTakesItOffEveryChat) {
  const File video = MakeFile("big", 6 * 1024 * 1024, "video/mp4");
  const File small = MakeFile("small", 3000);
  Keep(video);
  Keep(small);
  Post("A", "a1", video.fields);
  Post("B", "b1", video.fields);
  Post("A", "a2", small.fields);
  // The "larger than 5 MB" list: every (chat, hash) whose stored bytes are above 5 MB.
  std::vector<StoredAttachmentRef> large;
  for (const StoredAttachmentItem& item : workflow_->ListStoredAttachments(threads_)) {
    if (item.bytes > 5u * 1024 * 1024) {
      large.emplace_back(item.thread_id, item.content_hash);
    }
  }
  ASSERT_EQ(large.size(), 2u);
  const uint64_t stored = AttachmentLocalBytes(*content_, video.fields.content_hash);
  EXPECT_EQ(workflow_->ReleasableBytes(threads_, large), stored) << "counted once, though two chats hold it";

  const auto result = Delete(large);

  EXPECT_EQ(result.deleted, 2u);
  EXPECT_EQ(result.kept_shared, 0u);
  EXPECT_EQ(result.freed_bytes, stored);
  EXPECT_FALSE(Stored(video.fields));
  EXPECT_EQ(StateIn("A", video.fields), State::Pending);
  EXPECT_EQ(StateIn("B", video.fields), State::Pending);
  EXPECT_TRUE(Stored(small.fields));
  EXPECT_EQ(StateIn("A", small.fields), State::Ready);
}

TEST_F(AttachmentLocalDeleteTest, ARestartWithDownloadEverythingDoesNotBringItBackButATapDoes) {
  const File photo = MakeFile("again", 3000);
  Keep(photo);
  const ThreadMessage a1 = Post("A", "a1", photo.fields);
  Delete({{"A", photo.fields.content_hash}});

  // A new run: the suppression comes back from disk.
  workflow_.reset();
  suppression_ = std::make_unique<AttachmentSuppressionStore>(dir_.string());
  workflow_ = std::make_unique<AttachmentFetchWorkflow>();
  Wire(*workflow_, *suppression_);
  workflow_->SetDownloadPolicy(AttachmentDownloadPolicy::AlwaysAuto);
  workflow_->SetAutoAllowedOnNetwork(true);
  workflow_->EnqueueFromMessage("A", a1);
  workflow_->EnsureThreadQueued("A", threads_);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(Gets("again"), 0u);
  EXPECT_EQ(StateIn("A", photo.fields), State::Pending);

  workflow_->RequestDownload("A", "a1", threads_); // the card's Download
  ASSERT_TRUE(WaitFor([&] { return StateIn("A", photo.fields) == State::Ready; }));
  EXPECT_EQ(Gets("again"), 1u);
  EXPECT_TRUE(Stored(photo.fields));
  EXPECT_EQ(ItemsIn("A").size(), 1u);
}

TEST_F(AttachmentLocalDeleteTest, ATapOnAnItemAnotherChatKeptShowsItAtOnce) {
  const File photo = MakeFile("kept", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  Post("B", "b1", photo.fields);
  Delete({{"A", photo.fields.content_hash}});
  ASSERT_EQ(StateIn("A", photo.fields), State::Pending);

  workflow_->RequestDownload("A", "a1", threads_);

  EXPECT_EQ(StateIn("A", photo.fields), State::Ready);
  EXPECT_EQ(Gets("kept"), 0u) << "the bytes were here";
}

TEST_F(AttachmentLocalDeleteTest, ASendInFlightKeepsTheBytes) {
  const File photo = MakeFile("sending", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  workflow_->AcquireBytesForSend("C", photo.fields.content_hash); // stored, its message not written yet

  EXPECT_EQ(workflow_->ReleasableBytes(threads_, {{"A", photo.fields.content_hash}}), 0u);
  const auto result = Delete({{"A", photo.fields.content_hash}});
  Post("C", "c1", photo.fields);
  workflow_->ReleaseBytesForSend(photo.fields.content_hash);

  EXPECT_EQ(result.kept_shared, 1u);
  EXPECT_TRUE(Stored(photo.fields));
  EXPECT_EQ(StateIn("C", photo.fields), State::Ready);
  EXPECT_EQ(StateIn("A", photo.fields), State::Pending);
}

TEST_F(AttachmentLocalDeleteTest, ASendIntoTheChatItWasDeletedFromShowsItThereAgain) {
  const File photo = MakeFile("resent", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  Delete({{"A", photo.fields.content_hash}});
  ASSERT_FALSE(Stored(photo.fields));

  workflow_->AcquireBytesForSend("A", photo.fields.content_hash);
  Keep(photo);
  Post("A", "a2", photo.fields);
  workflow_->ReleaseBytesForSend(photo.fields.content_hash);

  EXPECT_EQ(StateIn("A", photo.fields), State::Ready);
}

// A delete racing a download, a send or a forward of the same bytes into another chat: nothing it should keep is
// deleted, nothing it deletes is left behind, a send never ends with a message whose bytes are gone.
TEST_F(AttachmentLocalDeleteTest, DeleteRacingADownloadASendOrAForwardOfTheSameBytes) {
  constexpr int kRounds = 200;
  int kept = 0;
  int freed = 0;
  int forward_refused = 0;
  for (int round = 0; round < kRounds; ++round) {
    const std::string name = "race" + std::to_string(round);
    const File file = MakeFile(name, 2000);
    Keep(file);
    Post("A", "a" + std::to_string(round), file.fields);
    const int op = round % 3;
    const std::string other = op == 2 ? "C" : "B";
    const std::string other_id = other + std::to_string(round);

    std::atomic<bool> go{false};
    StoredAttachmentDeleteResult result;
    std::thread deleter([&] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      result = Delete({{"A", file.fields.content_hash}});
    });
    std::thread racer([&] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      if (op == 0) {
        // A new message with the same file arrives in B and downloads by itself (its own sender: no pace limit).
        const ThreadMessage message = Post("B", other_id, file.fields, "sender" + std::to_string(round));
        workflow_->EnqueueFromMessage("B", message);
      } else if (op == 1) {
        // The same file sent into B: held, stored, its message written, released (as the hub's send does).
        workflow_->AcquireBytesForSend("B", file.fields.content_hash);
        ASSERT_TRUE(SaveAttachmentPlaintext(*content_, file.fields.content_hash, file.fields.mime, file.plain,
                                            file.fields.filename, Dek()));
        Post("B", other_id, file.fields, "me");
        workflow_->ReleaseBytesForSend(file.fields.content_hash);
      } else {
        // A's copy forwarded into C: held, read from the store, sent only when the bytes were there.
        workflow_->AcquireBytesForSend("C", file.fields.content_hash);
        if (auto plain = LoadAttachmentPlaintext(*content_, file.fields.content_hash, Dek())) {
          ASSERT_FALSE(plain->empty());
          ASSERT_TRUE(SaveAttachmentPlaintext(*content_, file.fields.content_hash, file.fields.mime, *plain,
                                              file.fields.filename, Dek()));
          Post("C", other_id, file.fields, "me");
        } else {
          ++forward_refused;
        }
        workflow_->ReleaseBytesForSend(file.fields.content_hash);
      }
    });
    go.store(true);
    deleter.join();
    racer.join();

    ASSERT_EQ(result.deleted, 1u) << "round " << round;
    EXPECT_EQ(StateIn("A", file.fields), State::Pending) << "round " << round;
    EXPECT_EQ(ViewFiles("A", file.fields), 0u) << "round " << round;
    auto message = threads_.GetMessage(other, other_id);
    ASSERT_TRUE(message);
    if (message->has_value()) {
      // The other chat's message stands: its bytes are there (kept, or downloaded again) and it shows them.
      ASSERT_TRUE(WaitFor([&] { return StateIn(other, file.fields) == State::Ready; })) << "round " << round;
      EXPECT_TRUE(Stored(file.fields)) << "round " << round;
    } else {
      // Only a forward that found the bytes gone sends nothing; then nothing uses them and none are left.
      ASSERT_EQ(op, 2) << "round " << round;
      EXPECT_FALSE(Stored(file.fields)) << "round " << round;
    }
    (result.kept_shared > 0 ? kept : freed) += 1;
  }
  std::cout << "[ race ] kept " << kept << ", freed " << freed << ", forwards refused " << forward_refused << "\n";
}

TEST_F(AttachmentLocalDeleteTest, AThumbnailBeingMadeWhenTheDeleteStartsLeavesNoFile) {
  const File photo = MakeFile("thumbed", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> calls{0};
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
    if (calls.fetch_add(1) == 0) {
      entered.set_value();
    }
    released.wait();
    return std::vector<uint8_t>(64, 0xd8);
  }});
  workflow_->RequestGridThumbnail("A", photo.fields.content_hash, photo.fields.mime, 1);
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);

  std::atomic<bool> deleted{false};
  std::thread deleter([&] {
    Delete({{"A", photo.fields.content_hash}});
    deleted = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(deleted.load()) << "the delete waits for the thumbnail being made";
  release.set_value();
  deleter.join();

  EXPECT_EQ(ViewFiles("A", photo.fields), 0u);
  EXPECT_TRUE(workflow_->GridThumbnailPath("A", photo.fields.content_hash).empty());
  EXPECT_FALSE(Stored(photo.fields));
  workflow_->RequestGridThumbnail("A", photo.fields.content_hash, photo.fields.mime, 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(calls.load(), 1) << "nothing is made for a deleted item";
}

// --- The storage grid's thumbnails --------------------------------------------------------------------------------

TEST_F(AttachmentLocalDeleteTest, GridThumbnailsAreMadeOnlyForTheCellsAskedFor) {
  std::atomic<int> calls{0};
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
    ++calls;
    return std::vector<uint8_t>(64, 0xd8);
  }});
  std::vector<File> photos;
  for (int i = 0; i < 10; ++i) {
    photos.push_back(MakeFile("cell" + std::to_string(i), 3000)); // small pictures: the chat makes no thumbnail
    Keep(photos.back());
    Post("A", "a" + std::to_string(i), photos.back().fields);
  }
  for (int i = 0; i < 3; ++i) {
    workflow_->RequestGridThumbnail("A", photos[i].fields.content_hash, photos[i].fields.mime, 7);
  }
  ASSERT_TRUE(WaitFor([&] {
    return std::all_of(photos.begin(), photos.begin() + 3, [&](const File& photo) {
      return !workflow_->GridThumbnailPath("A", photo.fields.content_hash).empty();
    });
  }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(calls.load(), 3);
  EXPECT_TRUE(workflow_->GridThumbnailPath("A", photos[3].fields.content_hash).empty());
  // Asked again with its thumbnail there: nothing more is made.
  workflow_->RequestGridThumbnail("A", photos[0].fields.content_hash, photos[0].fields.mime, 8);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(calls.load(), 3);
}

TEST_F(AttachmentLocalDeleteTest, AtMostTwoGridThumbnailsAreMadeAtOnce) {
  std::atomic<int> running{0};
  std::atomic<int> most{0};
  std::atomic<int> calls{0};
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
    const int now = ++running;
    int seen = most.load();
    while (now > seen && !most.compare_exchange_weak(seen, now)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    --running;
    ++calls;
    return std::vector<uint8_t>(64, 0xd8);
  }});
  std::vector<File> photos;
  for (int i = 0; i < 8; ++i) {
    photos.push_back(MakeFile("busy" + std::to_string(i), 3000));
    Keep(photos.back());
    workflow_->RequestGridThumbnail("A", photos.back().fields.content_hash, photos.back().fields.mime, 1);
  }
  ASSERT_TRUE(WaitFor([&] { return calls.load() == 8; }));
  EXPECT_LE(most.load(), 2);
  EXPECT_GE(most.load(), 1);
}

TEST_F(AttachmentLocalDeleteTest, ALeftPagesThumbnailsNotStartedNeverStart) {
  std::atomic<int> calls{0};
  std::promise<void> release;
  auto released = release.get_future().share();
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
    ++calls;
    released.wait();
    return std::vector<uint8_t>(64, 0xd8);
  }});
  for (int i = 0; i < 6; ++i) {
    const File photo = MakeFile("left" + std::to_string(i), 3000);
    Keep(photo);
    workflow_->RequestGridThumbnail("A", photo.fields.content_hash, photo.fields.mime, 3);
  }
  ASSERT_TRUE(WaitFor([&] { return calls.load() == 2; }));
  workflow_->CancelGridThumbnails(3); // back / another tab
  release.set_value();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(calls.load(), 2);
}

TEST_F(AttachmentLocalDeleteTest, AVideoWithoutAPosterIsNotDecryptedForTheGrid) {
  std::atomic<int> image_calls{0};
  std::atomic<int> video_calls{0};
  SetAttachmentPlatform({
      .video_poster_jpeg = [&](const std::string&, int) -> Roe<std::vector<uint8_t>> {
        ++video_calls;
        return std::vector<uint8_t>(64, 0xd8);
      },
      .image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
        ++image_calls;
        return std::vector<uint8_t>(64, 0xd8);
      },
  });
  const File video = MakeFile("clip", 3000, "video/mp4");
  Keep(video);
  Post("A", "a1", video.fields);

  workflow_->RequestGridThumbnail("A", video.fields.content_hash, video.fields.mime, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(image_calls.load(), 0);
  EXPECT_EQ(video_calls.load(), 0);
  EXPECT_TRUE(workflow_->GridThumbnailPath("A", video.fields.content_hash).empty());
  EXPECT_EQ(ViewFiles("A", video.fields), 0u) << "no plaintext copy was written";
}

// An AI image stored (StoreAiImage → StoreLocalImage) while a delete of the same bytes runs: the call returns at once,
// and the delete keeps the bytes it stored.
TEST_F(AttachmentLocalDeleteTest, StoringAnAiImageDuringADeleteOfTheSameBytesDoesNotWait) {
  const File photo = MakeFile("asked_again", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  // A thumbnail being made holds the delete in its first step (its mark is set) for as long as the test wants.
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> calls{0};
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
    if (calls.fetch_add(1) == 0) {
      entered.set_value();
    }
    released.wait();
    return std::vector<uint8_t>(64, 0xd8);
  }});
  workflow_->RequestGridThumbnail("A", photo.fields.content_hash, photo.fields.mime, 1);
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
  std::atomic<bool> deleted{false};
  StoredAttachmentDeleteResult result;
  std::thread deleter([&] {
    result = Delete({{"A", photo.fields.content_hash}});
    deleted = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_FALSE(deleted.load()) << "the delete is running";

  const auto started = std::chrono::steady_clock::now();
  auto stored = workflow_->StoreLocalImage("ai", photo.plain, "image/png");
  const auto took = std::chrono::steady_clock::now() - started;

  EXPECT_LT(took, std::chrono::milliseconds(500)) << "the calling thread does not wait for the delete";
  EXPECT_FALSE(deleted.load());
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->content_hash, photo.fields.content_hash);
  release.set_value();
  deleter.join();
  PostAiImage("q1", photo.fields); // the question's message, written after the store

  EXPECT_EQ(result.deleted, 1u);
  EXPECT_EQ(result.kept_shared, 1u) << "stored during the delete: kept";
  EXPECT_TRUE(Stored(photo.fields));
  EXPECT_EQ(StateIn("A", photo.fields), State::Pending) << "still deleted from A";
  EXPECT_TRUE(workflow_->EnsureLocalViewPath("ai", photo.fields.content_hash, "image/png", ""));
}

// Locked while a grid thumbnail is being made: nothing made from it stays, and the unlock makes it (the cell gets
// its thumbnail without being asked again). Only the grid's own callback hears of it, not the chats' change.
TEST_F(AttachmentLocalDeleteTest, LockedDuringAGridThumbnailTheUnlockMakesIt) {
  const File photo = MakeFile("relocked", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> calls{0};
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
    if (calls.fetch_add(1) == 0) {
      entered.set_value();
      released.wait();
    }
    return std::vector<uint8_t>(64, 0xd8);
  }});
  int changes = 0;
  std::vector<std::string> ready;
  workflow_->SetOnChanged([&] { ++changes; });
  workflow_->SetOnGridThumbnailReady([&](const std::string& thread_id, const std::vector<uint8_t>& hash) {
    ready.push_back(thread_id + "|" + AttachmentHashHex(hash));
  });

  workflow_->RequestGridThumbnail("A", photo.fields.content_hash, photo.fields.mime, 1);
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
  workflow_->ClearDek();
  release.set_value();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  AppRuntime::RunUITasks();
  EXPECT_TRUE(workflow_->GridThumbnailPath("A", photo.fields.content_hash).empty()) << "made while locked: not kept";
  EXPECT_TRUE(ready.empty());
  EXPECT_EQ(calls.load(), 1) << "nothing is made while locked";

  ASSERT_TRUE(workflow_->SetDek(Dek()));
  ASSERT_TRUE(WaitFor([&] {
    AppRuntime::RunUITasks();
    return !ready.empty();
  }));
  EXPECT_FALSE(workflow_->GridThumbnailPath("A", photo.fields.content_hash).empty());
  EXPECT_EQ(calls.load(), 2);
  EXPECT_EQ(ready, std::vector<std::string>{"A|" + AttachmentHashHex(photo.fields.content_hash)});
  EXPECT_EQ(changes, 0) << "a grid thumbnail is not an attachment change";
}

// A picture the hook cannot thumbnail is not tried again this session, until the next unlock.
TEST_F(AttachmentLocalDeleteTest, AGridThumbnailTheHookRefusedIsTriedAgainAfterTheNextUnlock) {
  const File photo = MakeFile("refused", 3000);
  Keep(photo);
  Post("A", "a1", photo.fields);
  std::atomic<int> calls{0};
  SetAttachmentPlatform({.image_thumbnail_jpeg = [&](const std::vector<uint8_t>&, int) -> Roe<std::vector<uint8_t>> {
    ++calls;
    return Error("cannot decode");
  }});
  workflow_->RequestGridThumbnail("A", photo.fields.content_hash, photo.fields.mime, 1);
  ASSERT_TRUE(WaitFor([&] { return calls.load() == 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  workflow_->RequestGridThumbnail("A", photo.fields.content_hash, photo.fields.mime, 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(calls.load(), 1) << "remembered as failed";

  workflow_->ClearDek();
  ASSERT_TRUE(workflow_->SetDek(Dek()));
  workflow_->RequestGridThumbnail("A", photo.fields.content_hash, photo.fields.mime, 3);
  EXPECT_TRUE(WaitFor([&] { return calls.load() == 2; })) << "forgotten at the unlock";
}

// --- What Me → Storage counts -------------------------------------------------------------------------------------

TEST_F(AttachmentLocalDeleteTest, TheListCountsTheBytesOnThisDeviceNotTheDeclaredSize) {
  File photo = MakeFile("declared", 20'000);
  Keep(photo);
  photo.fields.byte_length = 123; // what the sender wrote
  Post("A", "a1", photo.fields);

  const auto items = ItemsIn("A");
  ASSERT_EQ(items.size(), 1u);
  const uint64_t on_disk = std::filesystem::file_size(
      content_->BlockPath(CasRealm::Private, ByteVector(photo.fields.content_hash.begin(), photo.fields.content_hash.end())));
  EXPECT_EQ(items[0].bytes, on_disk);
  EXPECT_GT(items[0].bytes, 20'000u);
  EXPECT_EQ(items[0].message_id, "a1");
  EXPECT_EQ(items[0].mime, "image/png");
  EXPECT_TRUE(items[0].has_server_copy);
  EXPECT_EQ(items[0].timestamp_ms, 1000);
}

TEST_F(AttachmentLocalDeleteTest, TheListReadsEveryMessageOfALongChatAndSkipsWhatIsNotHere) {
  const File oldest = MakeFile("oldest", 3000);
  const File twice = MakeFile("twice", 3000);
  const File missing = MakeFile("missing", 3000);
  Keep(oldest);
  Keep(twice);
  File peer_only = MakeFile("peer", 3000);
  peer_only.fields.url.clear(); // came straight from the contact: no server copy
  Keep(peer_only);
  Post("A", "first", oldest.fields); // far behind the first pages
  Post("A", "early", twice.fields);
  Post("A", "gone", missing.fields); // not on this device
  Post("A", "direct", peer_only.fields);
  for (int i = 0; i < 1300; ++i) {
    ThreadMessage text;
    text.id = "t" + std::to_string(i);
    text.thread_id = "A";
    text.sender_contact_id = "alice";
    text.text = "hello";
    ASSERT_TRUE(threads_.AppendMessage(text));
  }
  Post("A", "again", twice.fields); // the same picture once more: one item, the newest message

  const auto items = ItemsIn("A");
  std::map<std::string, StoredAttachmentItem> by_message;
  for (const auto& item : items) {
    by_message[item.message_id] = item;
  }
  ASSERT_EQ(items.size(), 3u);
  EXPECT_TRUE(by_message.contains("first"));
  EXPECT_TRUE(by_message.contains("again"));
  ASSERT_TRUE(by_message.contains("direct"));
  EXPECT_FALSE(by_message["direct"].has_server_copy);
}

TEST_F(AttachmentLocalDeleteTest, ListingThreeThousandItemsInFiftyChatsIsQuick) {
  for (int chat = 0; chat < 50; ++chat) {
    Thread thread;
    thread.id = "chat" + std::to_string(chat);
    thread.kind = ThreadKind::Direct;
    ASSERT_TRUE(threads_.UpsertThread(thread));
    for (int i = 0; i < 60; ++i) {
      std::vector<uint8_t> plain(64, 0);
      plain[0] = static_cast<uint8_t>(chat);
      plain[1] = static_cast<uint8_t>(i);
      auto prepared = PrepareChatAttachmentFromBytes(plain, "f" + std::to_string(i) + ".bin", "", 1'000'000);
      ASSERT_TRUE(prepared);
      ChatAttachmentFields fields = prepared->fields;
      fields.mime = i % 3 == 0 ? "video/mp4" : (i % 3 == 1 ? "image/jpeg" : "application/pdf");
      fields.url = "https://example.invalid/f";
      ASSERT_TRUE(
          SaveAttachmentPlaintext(*content_, fields.content_hash, fields.mime, plain, fields.filename, Dek()));
      Post(thread.id, "m" + std::to_string(i), fields);
    }
  }
  const auto started = std::chrono::steady_clock::now();
  const auto items = workflow_->ListStoredAttachments(threads_);
  const auto took =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  EXPECT_EQ(items.size(), 3000u);
  EXPECT_LT(took, 200) << "listing took " << took << " ms";
  std::cout << "[ list ] 3000 items / 50 chats in " << took << " ms\n";
}

} // namespace
} // namespace pbr
