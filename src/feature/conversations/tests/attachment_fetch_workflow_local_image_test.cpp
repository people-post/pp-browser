#include "feature/conversations/AttachmentFetchWorkflow.h"

#include "feature/conversations/AiImageRef.h"
#include "foundation/crypto/CryptoConstants.h"
#include "domain/messaging/AttachmentPlaintextMemoryCache.h"
#include "domain/messaging/CasStore.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "common/thread/ThreadRecordTypes.h"
#include "common/Utilities.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

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
    workflow.SetProfileId("test");
  }
  ~LocalImageHarness() {
    std::error_code ec;
    std::filesystem::remove_all(data_dir, ec);
  }
  std::filesystem::path data_dir;
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

// Deleting the thread or clearing its history takes its AI images out of the CAS (nothing else holds them).
TEST(AttachmentFetchWorkflowLocalImageTest, DeleteLocalImagesRemovesCasObjectAndView) {
  LocalImageHarness h;
  ASSERT_TRUE(h.workflow.SetDek(TestDek()));
  const ByteVector bytes = {0xFF, 0xD8, 0xFF, 0xE0, 9, 8, 7, 6, 5};
  {
    // Windows CI: the store closes before the harness removes the directory.
    SqliteThreadStore store(h.data_dir.string());
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

    CasStore cas(h.data_dir.string(), "test");
    EXPECT_TRUE(cas.Exists(CasRealm::Private, stored->content_hash));
    EXPECT_TRUE(cas.Exists(CasRealm::Private, other->content_hash));
    ASSERT_TRUE(h.workflow.DeleteLocalImageOfMessage(thread.id, "msg-3", store));
    EXPECT_FALSE(cas.Exists(CasRealm::Private, other->content_hash));
    EXPECT_FALSE(std::filesystem::exists(other->view_path));
    EXPECT_TRUE(cas.Exists(CasRealm::Private, stored->content_hash));
    EXPECT_TRUE(std::filesystem::exists(stored->view_path));
    ASSERT_TRUE(h.workflow.DeleteLocalImageOfMessage(thread.id, "msg-2", store)); // no image: nothing happens
    EXPECT_TRUE(cas.Exists(CasRealm::Private, stored->content_hash));

    ASSERT_TRUE(h.workflow.DeleteLocalImages(thread.id, store));
    EXPECT_FALSE(cas.Exists(CasRealm::Private, stored->content_hash));
    EXPECT_FALSE(std::filesystem::exists(stored->view_path));
    EXPECT_FALSE(h.workflow.EnsureLocalViewPath(thread.id, stored->content_hash, "image/jpeg", std::string()));
    store.Flush();
  }
}

} // namespace
} // namespace pbr
