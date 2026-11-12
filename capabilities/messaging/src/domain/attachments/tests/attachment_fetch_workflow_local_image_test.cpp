#include "lib/content/testing/InMemoryObjectIndex.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"

#include "lib/messaging/AiImageRef.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/messaging/AttachmentPlaintextMemoryCache.h"
#include "lib/content/CasStore.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "common/thread/ThreadRecordTypes.h"
#include "common/Utilities.h"
#include "common/chat/MessagingLimits.h"
#include "common/runtime/AppRuntime.h"
#include "domain/attachments/AttachmentContent.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentPlatform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xb0 + i);
  }
  return dek;
}

ByteVector ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return ByteVector((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class LocalImageHarness {
public:
  LocalImageHarness()
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_ai_image_" + util::GenerateUuid())) {
    std::filesystem::create_directories(data_dir);
    workflow.SetProfileDataDir(data_dir.string());
    workflow.SetContent(content);
  }
  ~LocalImageHarness() {
    std::error_code ec;
    std::filesystem::remove_all(data_dir, ec);
  }
  std::filesystem::path data_dir;
  std::shared_ptr<CasStore> content = MakeInMemoryIndexedCasStore(data_dir.string(), "test");
  AttachmentFetchWorkflow workflow;
};

// An image sent to the AI is kept like a received attachment: after the view file is gone (restart, view
// wipe, memory cache gone) it comes back from the private CAS under the same path.
TEST(AttachmentFetchWorkflowLocalImageTest, StoredImageComesBackFromTheCas) {
  LocalImageHarness h;
  ASSERT_TRUE(h.workflow.SetDek(TestDek()));
  const ByteVector bytes = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

  auto stored = h.workflow.StoreLocalImage("thread-ai", bytes, "image/jpeg");
  ASSERT_TRUE(stored) << stored.error().message;
  EXPECT_EQ(stored->content_hash.size(), 32u);
  const std::filesystem::path view(stored->view_path);
  EXPECT_EQ(view.extension(), ".jpg");
  EXPECT_EQ(view.parent_path(), h.data_dir / "threads" / "thread-ai" / "blobs_view");
  EXPECT_EQ(ReadAll(stored->view_path), bytes);

  // Same bytes again: same object, same view.
  auto again = h.workflow.StoreLocalImage("thread-ai", bytes, "image/jpeg");
  ASSERT_TRUE(again);
  EXPECT_EQ(again->content_hash, stored->content_hash);
  EXPECT_EQ(again->view_path, stored->view_path);

  // View file and RAM copy gone (what a restart leaves behind): the CAS rebuilds the view.
  std::filesystem::remove(view);
  AttachmentPlaintextMemoryCache::Instance().Clear();
  auto rebuilt = h.workflow.EnsureLocalViewPath("thread-ai", stored->content_hash, "image/jpeg", std::string());
  ASSERT_TRUE(rebuilt) << rebuilt.error().message;
  EXPECT_EQ(*rebuilt, stored->view_path);
  EXPECT_EQ(ReadAll(*rebuilt), bytes);

  // Locked: nothing is stored or shown, and the views are wiped.
  h.workflow.ClearDek();
  EXPECT_FALSE(std::filesystem::exists(view));
  EXPECT_FALSE(h.workflow.StoreLocalImage("thread-ai", bytes, "image/jpeg"));
  EXPECT_FALSE(h.workflow.EnsureLocalViewPath("thread-ai", stored->content_hash, "image/jpeg", std::string()));
}

// Deleting the thread or clearing its history takes its media (AI images, attachments) out of the CAS.
TEST(AttachmentFetchWorkflowLocalImageTest, DeleteThreadMediaRemovesCasObjectsAndViews) {
  LocalImageHarness h;
  ASSERT_TRUE(h.workflow.SetDek(TestDek()));
  const ByteVector bytes = {0xFF, 0xD8, 0xFF, 0xE0, 9, 8, 7, 6, 5};
  {
    // Windows CI: the store closes before the harness removes the directory.
    InMemoryDeliveryStore store;
    ASSERT_TRUE(store.SetDek(TestDek()));
    Thread thread;
    thread.id = "thread-ai";
    thread.kind = ThreadKind::Ai;
    thread.title = "Assistant";
    ASSERT_TRUE(store.UpsertThread(thread));

    auto stored = h.workflow.StoreLocalImage(thread.id, bytes, "image/jpeg");
    ASSERT_TRUE(stored) << stored.error().message;
    ThreadMessage question;
    question.id = "msg-1";
    question.thread_id = thread.id;
    question.sender_contact_id = kLocalSelfContactId;
    question.text = "[Image] What is this?";
    question.timestamp = 1;
    question.ai_image_json = AiImageRefToJson(
        AiImageRef{.content_hash = stored->content_hash, .mime = "image/jpeg", .width = 4, .height = 3});
    ASSERT_TRUE(store.AppendMessage(question));
    ThreadMessage plain;
    plain.id = "msg-2";
    plain.thread_id = thread.id;
    plain.sender_contact_id = kLocalSelfContactId;
    plain.text = "no image";
    plain.timestamp = 2;
    ASSERT_TRUE(store.AppendMessage(plain));

    // The reference round-trips through the encrypted store.
    auto page = store.GetMessagesPage(thread.id, std::nullopt, 10);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 2u);
    ASSERT_TRUE(page->front().ai_image_json.has_value());
    EXPECT_EQ(*page->front().ai_image_json, *question.ai_image_json);
    EXPECT_FALSE(page->back().ai_image_json.has_value());

    // A second image question: "delete for me" on the first one takes only the first image away.
    const ByteVector other_bytes = {0xFF, 0xD8, 0xFF, 0xE0, 1, 1, 2, 3, 5, 8};
    auto other = h.workflow.StoreLocalImage(thread.id, other_bytes, "image/jpeg");
    ASSERT_TRUE(other);
    ThreadMessage second;
    second.id = "msg-3";
    second.thread_id = thread.id;
    second.sender_contact_id = kLocalSelfContactId;
    second.text = "[Image] And this?";
    second.timestamp = 3;
    second.ai_image_json = AiImageRefToJson(
        AiImageRef{.content_hash = other->content_hash, .mime = "image/jpeg", .width = 4, .height = 3});
    ASSERT_TRUE(store.AppendMessage(second));

    CasStore& cas = *h.content;
    EXPECT_TRUE(cas.Exists(CasRealm::Private, stored->content_hash));
    EXPECT_TRUE(cas.Exists(CasRealm::Private, other->content_hash));
    h.workflow.DeleteMessagesMedia(thread.id, {second}, {});
    EXPECT_FALSE(cas.Exists(CasRealm::Private, other->content_hash));
    EXPECT_FALSE(std::filesystem::exists(other->view_path));
    EXPECT_TRUE(cas.Exists(CasRealm::Private, stored->content_hash));
    EXPECT_TRUE(std::filesystem::exists(stored->view_path));
    h.workflow.DeleteMessagesMedia(thread.id, {plain}, {}); // no image: nothing happens
    // Another live message of the thread still shows the same bytes: they stay.
    h.workflow.DeleteMessagesMedia(thread.id, {question}, {question});
    EXPECT_TRUE(cas.Exists(CasRealm::Private, stored->content_hash));
    EXPECT_TRUE(cas.Exists(CasRealm::Private, stored->content_hash));

    // A chat attachment row: its object goes with the thread too.
    const ByteVector file_bytes = {0x25, 0x50, 0x44, 0x46, 1, 2, 3, 4};
    auto file = h.workflow.StoreLocalImage(thread.id, file_bytes, "application/pdf"); // same CAS, any bytes
    ASSERT_TRUE(file);
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/blob";
    fields.mime = "application/pdf";
    fields.filename = "a.pdf";
    fields.byte_length = file_bytes.size();
    fields.content_hash = file->content_hash;
    fields.content_key.assign(32, 0x11);
    fields.blob_nonce.assign(24, 0x22);
    ThreadMessage attachment;
    attachment.id = "msg-4";
    attachment.thread_id = thread.id;
    attachment.sender_contact_id = kLocalSelfContactId;
    attachment.content_type = ChatContentType::Attachment;
    attachment.text = "a.pdf";
    attachment.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    attachment.timestamp = 4;
    ASSERT_TRUE(store.AppendMessage(attachment));
    EXPECT_TRUE(cas.Exists(CasRealm::Private, file->content_hash));

    // "Clear downloaded media" (and the first step of clear history) drops views only: the messages stay,
    // and so do their only copies.
    ASSERT_TRUE(h.workflow.PrepareThreadHistoryClear(thread.id, store));
    EXPECT_TRUE(cas.Exists(CasRealm::Private, stored->content_hash));
    EXPECT_TRUE(cas.Exists(CasRealm::Private, file->content_hash));

    ASSERT_TRUE(h.workflow.DeleteThreadMedia(thread.id, store));
    EXPECT_FALSE(cas.Exists(CasRealm::Private, file->content_hash));
    EXPECT_FALSE(cas.Exists(CasRealm::Private, stored->content_hash));
    EXPECT_FALSE(std::filesystem::exists(stored->view_path));
    EXPECT_FALSE(h.workflow.EnsureLocalViewPath(thread.id, stored->content_hash, "image/jpeg", std::string()));
    store.Flush();
  }
}


// --- Chat picture previews (B203, DEV/PLAN-chat-picture-previews.md) ---------------------------------------------

bool WaitUntil(const std::function<bool()>& done, const std::chrono::milliseconds budget = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    AppRuntime::RunUITasks();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

/** A PNG's first bytes declaring `w` x `h`, then filler up to `size` bytes; `tag` makes the bytes (and hash) unique. */
ByteVector PngOf(const uint32_t w, const uint32_t h, const size_t size, const uint8_t tag) {
  ByteVector bytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                      static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8),
                      static_cast<uint8_t>(w), static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16),
                      static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h), 8, 2, 0, 0, 0};
  bytes.resize(std::max(size, bytes.size()), tag);
  bytes.push_back(tag);
  return bytes;
}

/** A workflow with a running runtime and a counting thumbnail hook (the tag is the picture's last byte). */
class ChatPreviewTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_chat_preview_" + util::GenerateUuid());
    std::filesystem::create_directories(data_dir_);
    content_ = MakeInMemoryIndexedCasStore(data_dir_.string(), "test");
    workflow_.SetProfileDataDir(data_dir_.string());
    workflow_.SetContent(content_);
    ASSERT_TRUE(workflow_.SetDek(TestDek()));
    workflow_.SetOnChatPreviewDone([this](const std::string&, const std::vector<uint8_t>& hash, const bool made) {
      std::lock_guard lock(mu_);
      (made ? made_ : failed_).push_back(AttachmentHashHex(hash));
    });
    SetAttachmentPlatform({.image_thumbnail_jpeg = [this](const std::vector<uint8_t>& bytes,
                                                          int max_dimension) -> Roe<std::vector<uint8_t>> {
      const int now = ++running_;
      int seen = max_running_.load();
      while (now > seen && !max_running_.compare_exchange_weak(seen, now)) {
      }
      {
        std::lock_guard lock(mu_);
        started_.push_back(bytes.back());
        sizes_.push_back(max_dimension);
      }
      gate_.wait();
      --running_;
      if (fail_) {
        return Error("cannot decode");
      }
      return std::vector<uint8_t>{0xff, 0xd8, 0xff, 0xd9};
    }});
    hold_gate();
    open_gate();
  }
  void TearDown() override {
    open_gate();
    AppRuntime::Shutdown(); // joins the workers before the workflow goes
    SetAttachmentPlatform({});
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
  }
  void hold_gate() {
    release_ = std::promise<void>();
    gate_ = release_.get_future().share();
  }
  void open_gate() {
    try {
      release_.set_value();
    } catch (const std::future_error&) {
    }
  }
  /** Stores `bytes` as a picture of this thread and returns its attachment fields. */
  ChatAttachmentFields Store(const ByteVector& bytes) {
    auto hash = AttachmentContentHash(bytes);
    EXPECT_TRUE(hash);
    EXPECT_TRUE(SaveAttachmentPlaintext(*content_, *hash, "image/png", bytes, "pic.png", TestDek()));
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/pic";
    fields.mime = "image/png";
    fields.filename = "pic.png";
    fields.byte_length = bytes.size();
    fields.content_hash = *hash;
    fields.content_key.assign(32, 0x11);
    fields.blob_nonce.assign(24, 0x22);
    return fields;
  }
  ThreadMessage MessageOf(const std::string& id, const ChatAttachmentFields& fields) {
    ThreadMessage message;
    message.id = id;
    message.thread_id = "t1";
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return message;
  }
  bool PreviewExists(const ChatAttachmentFields& fields) const {
    return AttachmentPosterExists(data_dir_.string(), "t1", fields.content_hash);
  }
  std::vector<uint8_t> Started() {
    std::lock_guard lock(mu_);
    return started_;
  }
  size_t MadeCount() {
    std::lock_guard lock(mu_);
    return made_.size();
  }
  size_t FailedCount() {
    std::lock_guard lock(mu_);
    return failed_.size();
  }

  std::filesystem::path data_dir_;
  std::shared_ptr<CasStore> content_;
  AttachmentFetchWorkflow workflow_;
  std::promise<void> release_;
  std::shared_future<void> gate_;
  std::atomic<int> running_{0};
  std::atomic<int> max_running_{0};
  std::atomic<bool> fail_{false};
  std::mutex mu_;
  std::vector<uint8_t> started_;
  std::vector<int> sizes_;
  std::vector<std::string> made_;
  std::vector<std::string> failed_;
};

// A picture gets a preview when its long side is above kImagePreviewMaxSide (640), whatever its file size; a picture
// no longer than that is shown from itself; above the direct size the size is not read, so one is always made.
TEST_F(ChatPreviewTest, ThePreviewIsMadeForAPictureLongerThanThePreviewOnly) {
  const ByteVector tall = PngOf(1000, 800, 50'000, 1);
  const ByteVector fits = PngOf(640, 480, 50'000, 2);
  const ByteVector large_small_side = PngOf(600, 400, static_cast<size_t>(kMaxDirectAttachmentBytes) + 10, 3);
  const ByteVector unreadable(50'000, 4);
  const ChatAttachmentFields tall_fields = Store(tall);
  const ChatAttachmentFields fits_fields = Store(fits);
  const ChatAttachmentFields large_fields = Store(large_small_side);
  ChatAttachmentFields unreadable_fields = Store(unreadable);
  workflow_.MaybeBuildImageThumbnail("t1", tall_fields, tall);
  workflow_.MaybeBuildImageThumbnail("t1", fits_fields, fits);
  workflow_.MaybeBuildImageThumbnail("t1", large_fields, large_small_side);
  workflow_.MaybeBuildImageThumbnail("t1", unreadable_fields, unreadable);
  EXPECT_TRUE(PreviewExists(tall_fields));
  EXPECT_FALSE(PreviewExists(fits_fields)) << "640 on its long side: shown from itself";
  EXPECT_TRUE(PreviewExists(large_fields)) << "a large file's size is not read: always a preview";
  EXPECT_FALSE(PreviewExists(unreadable_fields)) << "an unreadable head is a file card, never decoded";
  std::lock_guard lock(mu_);
  ASSERT_EQ(sizes_.size(), 2u);
  EXPECT_EQ(sizes_[0], kImagePreviewMaxSide);
  EXPECT_EQ(kImagePreviewMaxSide, 640);
}

// At most two at once, the newest request first (the rows are drawn oldest first: the last asked for are on screen).
TEST_F(ChatPreviewTest, AtMostTwoAtOnceAndTheNewestRequestFirst) {
  std::vector<ChatAttachmentFields> pictures;
  for (uint8_t i = 0; i < 6; ++i) {
    pictures.push_back(Store(PngOf(2000, 1500, 10'000, static_cast<uint8_t>(10 + i))));
  }
  workflow_.ClearDek(); // the requests wait: their order is then the queue's alone
  for (const ChatAttachmentFields& fields : pictures) {
    workflow_.RequestChatPreview("t1", fields);
  }
  hold_gate();
  ASSERT_TRUE(workflow_.SetDek(TestDek()));
  ASSERT_TRUE(WaitUntil([&] { return Started().size() == 2; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(Started().size(), 2u) << "no third while two run";
  std::vector<uint8_t> first = Started();
  std::sort(first.begin(), first.end());
  EXPECT_EQ(first, (std::vector<uint8_t>{14, 15})) << "the two newest first";
  open_gate();
  ASSERT_TRUE(WaitUntil([&] { return MadeCount() == 6; }));
  EXPECT_EQ(max_running_.load(), 2);
  EXPECT_EQ(Started().back(), 10) << "the oldest last";
}

// Leaving the chat drops its requests not started; the storage grid's queue and cancel are its own.
TEST_F(ChatPreviewTest, LeavingTheChatCancelsOnlyItsOwnRequestsAndTheyCanBeAskedAgain) {
  const ChatAttachmentFields chat_a = Store(PngOf(2000, 1500, 10'000, 20));
  const ChatAttachmentFields chat_b = Store(PngOf(2000, 1500, 10'000, 21));
  const ChatAttachmentFields grid_a = Store(PngOf(2000, 1500, 10'000, 22));
  const ChatAttachmentFields grid_b = Store(PngOf(2000, 1500, 10'000, 23));
  workflow_.ClearDek();
  workflow_.RequestChatPreview("t1", chat_a);
  workflow_.RequestChatPreview("t1", chat_b);
  workflow_.RequestGridThumbnail("t1", grid_a.content_hash, grid_a.mime, 7);
  workflow_.RequestGridThumbnail("t1", grid_b.content_hash, grid_b.mime, 7);
  workflow_.CancelChatPreviews();
  workflow_.CancelGridThumbnails(8); // another grid page: nothing of page 7 goes
  ASSERT_TRUE(workflow_.SetDek(TestDek()));
  ASSERT_TRUE(WaitUntil([&] { return PreviewExists(grid_a) && PreviewExists(grid_b); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(PreviewExists(chat_a));
  EXPECT_FALSE(PreviewExists(chat_b));
  EXPECT_FALSE(workflow_.ChatPreviewFailed("t1", chat_a.content_hash)) << "cancelled is not failed";
  workflow_.RequestChatPreview("t1", chat_a); // its row shows again
  ASSERT_TRUE(WaitUntil([&] { return PreviewExists(chat_a) && MadeCount() == 1; })) << "the chat is told";
}

TEST_F(ChatPreviewTest, TheGridsOrderIsKeptWhileTheChatAsks) {
  std::vector<ChatAttachmentFields> grid;
  for (uint8_t i = 0; i < 4; ++i) {
    grid.push_back(Store(PngOf(2000, 1500, 10'000, static_cast<uint8_t>(30 + i))));
  }
  workflow_.ClearDek();
  for (const ChatAttachmentFields& fields : grid) {
    workflow_.RequestGridThumbnail("t1", fields.content_hash, fields.mime, 1);
  }
  workflow_.CancelChatPreviews(); // the chat left: nothing of the grid goes
  ASSERT_TRUE(workflow_.SetDek(TestDek()));
  ASSERT_TRUE(WaitUntil([&] { return Started().size() == 4; }));
  const std::vector<uint8_t> order = Started();
  EXPECT_EQ(std::min(order[0], order[1]), 30);
  EXPECT_EQ(std::max(order[2], order[3]), 33) << "the grid still goes in order";
}

TEST_F(ChatPreviewTest, NothingIsMadeWhileLockedAndItIsMadeAfterTheUnlock) {
  const ChatAttachmentFields fields = Store(PngOf(2000, 1500, 10'000, 40));
  workflow_.ClearDek();
  workflow_.RequestChatPreview("t1", fields);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_TRUE(Started().empty());
  ASSERT_TRUE(workflow_.SetDek(TestDek()));
  ASSERT_TRUE(WaitUntil([&] { return PreviewExists(fields) && MadeCount() == 1; }));
}

// A lock while a preview is being made: what it made is taken back (the view cache was wiped), it is not a failure,
// and nothing derived from the plaintext is left in memory.
TEST_F(ChatPreviewTest, ALockWhileMakingLeavesNoPreviewAndNoPlaintextInMemory) {
  const ByteVector small = PngOf(1000, 800, 10'000, 50);
  const ChatAttachmentFields small_fields = Store(small);
  ASSERT_TRUE(workflow_.EnsureLocalViewPath("t1", small_fields.content_hash, "image/png", "pic.png"));
  const ChatAttachmentFields fields = Store(PngOf(2000, 1500, 10'000, 51));
  hold_gate();
  workflow_.RequestChatPreview("t1", fields);
  ASSERT_TRUE(WaitUntil([&] { return Started().size() == 1; }));
  workflow_.ClearDek();
  EXPECT_EQ(AttachmentPlaintextMemoryCache::Instance().EntryCountForTest(), 0u);
  open_gate();
  ASSERT_TRUE(WaitUntil([&] { return running_.load() == 0; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  AppRuntime::RunUITasks();
  EXPECT_FALSE(PreviewExists(fields));
  EXPECT_FALSE(workflow_.ChatPreviewFailed("t1", fields.content_hash));
  EXPECT_EQ(MadeCount(), 0u);
  EXPECT_EQ(FailedCount(), 0u);
}

// The same for the preview made from the bytes in hand after a download or a send (MaybeBuildImageThumbnail, every
// picture longer than 640 px): a lock while it is written leaves nothing in the view cache.
TEST_F(ChatPreviewTest, ALockWhileMakingFromTheBytesInHandLeavesNoPreview) {
  const ByteVector bytes = PngOf(1000, 800, 10'000, 52);
  const ChatAttachmentFields fields = Store(bytes);
  hold_gate();
  std::thread maker([&] { workflow_.MaybeBuildImageThumbnail("t1", fields, bytes); });
  const bool started = WaitUntil([&] { return Started().size() == 1; });
  workflow_.ClearDek(); // the view cache is wiped now, before the preview is written
  open_gate();
  maker.join();
  ASSERT_TRUE(started);
  EXPECT_FALSE(PreviewExists(fields)) << "written after the wipe: taken back";

  // Unlocked again while it was written (another key generation): taken back too, made at the next drawing.
  ASSERT_TRUE(workflow_.SetDek(TestDek()));
  hold_gate();
  std::thread again([&] { workflow_.MaybeBuildImageThumbnail("t1", fields, bytes); });
  const bool started_again = WaitUntil([&] { return Started().size() == 2; });
  workflow_.ClearDek();
  EXPECT_TRUE(workflow_.SetDek(TestDek()));
  open_gate();
  again.join();
  ASSERT_TRUE(started_again);
  EXPECT_FALSE(PreviewExists(fields));

  workflow_.MaybeBuildImageThumbnail("t1", fields, bytes); // nothing in between: kept
  EXPECT_TRUE(PreviewExists(fields));
}

// Each page's cancel takes only its own requests: the chat leaving keeps the storage grid's, and the grid's page
// cancelled keeps the chat's.
TEST_F(ChatPreviewTest, TheGridsCancelAndTheChatsCancelEachTakeOnlyTheirOwn) {
  const ChatAttachmentFields chat_a = Store(PngOf(2000, 1500, 10'000, 80));
  const ChatAttachmentFields grid_a = Store(PngOf(2000, 1500, 10'000, 81));
  const ChatAttachmentFields grid_b = Store(PngOf(2000, 1500, 10'000, 82));
  workflow_.ClearDek();
  workflow_.RequestGridThumbnail("t1", grid_a.content_hash, grid_a.mime, 7);
  workflow_.RequestChatPreview("t1", chat_a);
  workflow_.CancelChatPreviews();     // the chat left
  workflow_.RequestChatPreview("t1", chat_a); // and opened again
  workflow_.RequestGridThumbnail("t1", grid_b.content_hash, grid_b.mime, 7);
  workflow_.CancelGridThumbnails(7); // the grid page closed
  ASSERT_TRUE(workflow_.SetDek(TestDek()));
  ASSERT_TRUE(WaitUntil([&] { return PreviewExists(chat_a) && MadeCount() == 1; })) << "the chat's request stays";
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(PreviewExists(grid_a));
  EXPECT_FALSE(PreviewExists(grid_b));
  EXPECT_EQ(Started().size(), 1u);
}

// Bytes the platform cannot make a preview from: remembered (its row is a file card), not tried again, said once.
TEST_F(ChatPreviewTest, APreviewThatCannotBeMadeIsRememberedAndNotTriedAgain) {
  const ChatAttachmentFields fields = Store(PngOf(2000, 1500, 10'000, 60));
  fail_ = true;
  workflow_.RequestChatPreview("t1", fields);
  ASSERT_TRUE(WaitUntil([&] { return FailedCount() == 1; }));
  EXPECT_TRUE(workflow_.ChatPreviewFailed("t1", fields.content_hash));
  workflow_.RequestChatPreview("t1", fields);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(Started().size(), 1u);
}

// "Delete for me": the message's preview and view go with it, a small picture's and a large one's; the same picture
// sent twice in the chat keeps its files while the other message shows it.
TEST_F(ChatPreviewTest, DeletingAMessageTakesItsPreviewUnlessAnotherMessageShowsTheSameFile) {
  const ByteVector small = PngOf(1000, 800, 10'000, 70);
  const ByteVector large = PngOf(4000, 3000, static_cast<size_t>(kMaxDirectAttachmentBytes) + 10, 71);
  const ChatAttachmentFields small_fields = Store(small);
  const ChatAttachmentFields large_fields = Store(large);
  workflow_.MaybeBuildImageThumbnail("t1", small_fields, small);
  workflow_.MaybeBuildImageThumbnail("t1", large_fields, large);
  auto view = workflow_.EnsureLocalViewPath("t1", small_fields.content_hash, "image/png", "pic.png");
  ASSERT_TRUE(view);
  ASSERT_TRUE(PreviewExists(small_fields));
  ASSERT_TRUE(PreviewExists(large_fields));

  // Sent twice: deleting one keeps what the other shows.
  const ThreadMessage first = MessageOf("m1", small_fields);
  const ThreadMessage second = MessageOf("m2", small_fields);
  workflow_.DeleteMessagesMedia("t1", {first}, {second});
  EXPECT_TRUE(PreviewExists(small_fields));
  EXPECT_TRUE(std::filesystem::exists(*view));

  workflow_.DeleteMessagesMedia("t1", {second}, {});
  EXPECT_FALSE(PreviewExists(small_fields));
  EXPECT_FALSE(std::filesystem::exists(*view));
  workflow_.DeleteMessagesMedia("t1", {MessageOf("m3", large_fields)}, {});
  EXPECT_FALSE(PreviewExists(large_fields));
}

} // namespace
} // namespace pbr
