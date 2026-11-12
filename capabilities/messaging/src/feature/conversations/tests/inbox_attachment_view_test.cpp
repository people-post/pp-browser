#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "feature/conversations/InboxController.h"
#include "common/chat/MessagingLimits.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"
#include "domain/attachments/AttachmentContent.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "lib/content/testing/InMemoryObjectIndex.h"
#include "lib/messaging/AttachmentPlatform.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <map>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

// A picture received from a contact is shown inline only when its declared size is within the pixel cap;
// otherwise the card is a file row and the bytes are never decoded.
class InboxAttachmentViewTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_inbox_view_" + util::GenerateUuid());
    std::filesystem::create_directories(data_dir_);
    store_ = std::make_unique<InMemoryDeliveryStore>();
    ASSERT_TRUE(store_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    shadows_ = std::make_unique<DirectoryShadowCache>(directory_);
    labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, PeerDisplaySourcesFrom(*shadows_));
    inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *labels_, shadows_.get());
    inbox_->SetProfileDataDir(data_dir_.string());
  }
  void TearDown() override {
    inbox_.reset();
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
  }

  /** A received picture whose view file (`blobs_view/<hash>.png`) holds `bytes`. */
  TranscriptAttachmentView ViewOf(const uint8_t seed, const std::vector<uint8_t>& bytes, const uint64_t declared = 0,
                                  const bool write_view = true) {
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/pic";
    fields.mime = "image/png";
    fields.filename = "pic.png";
    fields.byte_length = declared > 0 ? declared : bytes.size();
    fields.content_hash.assign(32, seed);
    fields.content_key.assign(kSessionKeySize, 1);
    fields.blob_nonce.assign(kAeadNonceSize, 2);
    const auto view_dir = std::filesystem::path(AttachmentViewRoot(data_dir_.string(), "t1"));
    std::filesystem::create_directories(view_dir);
    if (write_view) {
      std::ofstream out(view_dir / (AttachmentHashHex(fields.content_hash) + ".png"), std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ThreadMessage message;
    message.id = "m" + std::to_string(seed);
    message.thread_id = "t1";
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return inbox_->Facts().attachment(message);
  }

  static std::vector<uint8_t> PngHeader(const uint32_t w, const uint32_t h) {
    return {0x89,
            'P',
            'N',
            'G',
            0x0D,
            0x0A,
            0x1A,
            0x0A,
            0,
            0,
            0,
            13,
            'I',
            'H',
            'D',
            'R',
            static_cast<uint8_t>(w >> 24),
            static_cast<uint8_t>(w >> 16),
            static_cast<uint8_t>(w >> 8),
            static_cast<uint8_t>(w),
            static_cast<uint8_t>(h >> 24),
            static_cast<uint8_t>(h >> 16),
            static_cast<uint8_t>(h >> 8),
            static_cast<uint8_t>(h),
            8,
            2,
            0,
            0,
            0};
  }

  std::filesystem::path data_dir_;
  MockDirectoryClient directory_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<DirectoryShadowCache> shadows_;
  std::unique_ptr<PeerDisplayResolver> labels_;
  std::unique_ptr<InboxController> inbox_;
};

TEST_F(InboxAttachmentViewTest, AHeaderWithinTheCapIsShownInline) {
  const auto view = ViewOf(1, PngHeader(600, 600)); // no longer than the preview: shown from itself
  EXPECT_FALSE(view.local_path.empty());
  EXPECT_TRUE(view.inline_image_ok);
  EXPECT_FALSE(view.picture_placeholder);
}

TEST_F(InboxAttachmentViewTest, AHeaderClaiming30000By30000IsAFileRowAndNeverDecoded) {
  const auto view = ViewOf(2, PngHeader(30000, 30000));
  EXPECT_FALSE(view.local_path.empty()) << "it can still be opened by the system viewer";
  EXPECT_FALSE(view.inline_image_ok);
}

TEST_F(InboxAttachmentViewTest, AnUnreadableHeadIsAFileRowToo) {
  const auto view = ViewOf(3, {'n', 'o', 't', ' ', 'a', ' ', 'p', 'i', 'c', 't', 'u', 'r', 'e'});
  EXPECT_FALSE(view.inline_image_ok);
}

// A file above the direct size is never decrypted by drawing the card: no view, a file row / video placeholder with
// Open, until an Open has made the view for this session.
TEST_F(InboxAttachmentViewTest, ALargeFileHasNoViewUntilItIsOpened) {
  const uint64_t large = kMaxDirectAttachmentBytes + 1;
  const auto before = ViewOf(4, PngHeader(10, 10), large, /*write_view=*/false);
  EXPECT_TRUE(before.on_demand);
  EXPECT_TRUE(before.local_path.empty());
  EXPECT_TRUE(before.inline_image_ok);

  // An Open made the full picture's view: the bubble still never shows it (B203), it waits for the preview.
  const auto after = ViewOf(4, PngHeader(10, 10), large, /*write_view=*/true);
  EXPECT_TRUE(after.on_demand);
  EXPECT_TRUE(after.local_path.empty());
  EXPECT_TRUE(after.picture_placeholder);

  const auto small = ViewOf(5, PngHeader(10, 10), 1000, /*write_view=*/true);
  EXPECT_FALSE(small.on_demand);
}

// A large picture whose thumbnail was made shows the thumbnail inline; `on_demand` stays so a tap opens the original.
TEST_F(InboxAttachmentViewTest, ALargePictureWithAThumbnailShowsItInlineAndStaysOnDemand) {
  const uint64_t large = kMaxDirectAttachmentBytes + 1;
  const auto without = ViewOf(6, PngHeader(10, 10), large, /*write_view=*/false);
  EXPECT_TRUE(without.local_path.empty()) << "no preview yet: its placeholder";
  EXPECT_TRUE(without.picture_placeholder);

  const std::vector<uint8_t> hash(32, 6);
  const std::string thumb = AttachmentPosterPath(data_dir_.string(), "t1", hash);
  {
    const auto head = PngHeader(480, 360);
    std::ofstream out(thumb, std::ios::binary);
    out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
  }
  const auto with = ViewOf(6, PngHeader(10, 10), large, /*write_view=*/false);
  EXPECT_EQ(with.local_path, thumb);
  EXPECT_TRUE(with.inline_image_ok);
  EXPECT_TRUE(with.on_demand);

  // Up to the direct size nothing changes: a small picture is shown from its own view, never a thumbnail.
  const auto small = ViewOf(7, PngHeader(10, 10), 1000, /*write_view=*/true);
  EXPECT_FALSE(small.on_demand);
  EXPECT_EQ(small.local_path.find(".poster.jpg"), std::string::npos);
}

// After a restart the view cache (and the thumbnail in it) is gone: drawing the card asks for it again, it is made on
// a worker (once, however often the card is drawn) and the next draw shows it.
TEST_F(InboxAttachmentViewTest, AStoredLargePictureWithoutItsThumbnailGetsItMadeAgainAndThenShowsIt) {
  const auto content = MakeInMemoryIndexedCasStore(data_dir_.string(), "p1");
  AttachmentFetchWorkflow downloads;
  struct Runtime { // after `downloads`, so its workers are joined before it goes
    Runtime() { AppRuntime::Initialize(); }
    ~Runtime() {
      AppRuntime::Shutdown();
      SetAttachmentPlatform({});
    }
  } runtime;
  downloads.SetProfileDataDir(data_dir_.string());
  downloads.SetContent(content);
  ASSERT_TRUE(downloads.SetDek(TestDek()));
  std::atomic<int> changed{0};
  downloads.SetOnChatPreviewDone(
      [&changed](const std::string&, const std::vector<uint8_t>&, const bool made) { changed += made ? 1 : 0; });
  inbox_->SetAttachmentDownloads(&downloads);

  std::vector<uint8_t> plain(static_cast<size_t>(kMaxDirectAttachmentBytes) + 1000, 0x37);
  auto hash = AttachmentContentHash(plain);
  ASSERT_TRUE(hash);
  ASSERT_TRUE(SaveAttachmentPlaintext(*content, *hash, "image/png", plain, "pic.png", TestDek()));
  ChatAttachmentFields fields;
  fields.url = "https://cdn.example/pic";
  fields.mime = "image/png";
  fields.filename = "pic.png";
  fields.byte_length = plain.size();
  fields.content_hash = *hash;
  fields.content_key.assign(kSessionKeySize, 1);
  fields.blob_nonce.assign(kAeadNonceSize, 2);
  ThreadMessage message;
  message.id = "big";
  message.thread_id = "t1";
  message.content_type = ChatContentType::Attachment;
  message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);

  std::atomic<int> calls{0};
  std::atomic<bool> same_bytes{false};
  SetAttachmentPlatform(
      {.image_thumbnail_jpeg = [&](const std::vector<uint8_t>& bytes, int) -> Roe<std::vector<uint8_t>> {
        ++calls;
        same_bytes = bytes == plain;
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // re-draws arrive while it is being made
        return std::vector<uint8_t>(PngHeader(480, 360));
      }});

  const auto first = inbox_->Facts().attachment(message);
  EXPECT_EQ(first.state, AttachmentFetchWorkflow::DownloadState::Ready);
  EXPECT_TRUE(first.local_path.empty()) << "its placeholder until the preview exists";
  EXPECT_TRUE(first.picture_placeholder);
  EXPECT_EQ(first.image_width, kPicturePlaceholderFallbackWidth) << "the large file is not decrypted for its size";
  EXPECT_EQ(first.image_height, kPicturePlaceholderFallbackHeight);
  EXPECT_TRUE(first.on_demand);
  (void)inbox_->Facts().attachment(message);
  (void)inbox_->Facts().attachment(message);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (changed.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    AppRuntime::RunUITasks();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_GT(changed.load(), 0) << "the chat is told to redraw its rows";
  const auto second = inbox_->Facts().attachment(message);
  EXPECT_EQ(second.local_path, AttachmentPosterPath(data_dir_.string(), "t1", *hash));
  EXPECT_TRUE(second.inline_image_ok);
  EXPECT_FALSE(second.picture_placeholder);
  EXPECT_EQ(second.image_width, 480) << "a large picture's size is its preview's";
  EXPECT_EQ(second.image_height, 360);
  EXPECT_TRUE(second.on_demand) << "a tap still opens the original";
  EXPECT_EQ(calls.load(), 1) << "made once";
  EXPECT_TRUE(same_bytes.load());

  inbox_->SetAttachmentDownloads(nullptr);
}


// --- B203: the bubble shows a preview or a placeholder, never the full picture --------------------------------------

/** Pictures stored in a real workflow (encrypted store, DEK, workers), so drawing a card can ask for its preview. */
class InboxPicturePreviewTest : public InboxAttachmentViewTest {
protected:
  void SetUp() override {
    InboxAttachmentViewTest::SetUp();
    AppRuntime::Initialize();
    content_ = MakeInMemoryIndexedCasStore(data_dir_.string(), "p1");
    downloads_ = std::make_unique<AttachmentFetchWorkflow>();
    downloads_->SetProfileDataDir(data_dir_.string());
    downloads_->SetContent(content_);
    ASSERT_TRUE(downloads_->SetDek(TestDek()));
    downloads_->SetOnChatPreviewDone([this](const std::string&, const std::vector<uint8_t>&, const bool made) {
      (made ? made_ : failed_)++;
    });
    SetAttachmentPlatform({.image_thumbnail_jpeg = [this](const std::vector<uint8_t>&,
                                                          int) -> Roe<std::vector<uint8_t>> {
      ++calls_;
      if (fail_) {
        return Error("cannot decode");
      }
      return PngHeader(640, 512);
    }});
    inbox_->SetAttachmentDownloads(downloads_.get());
  }
  void TearDown() override {
    inbox_->SetAttachmentDownloads(nullptr);
    AppRuntime::Shutdown(); // its workers are joined before the workflow goes
    downloads_.reset();
    SetAttachmentPlatform({});
    InboxAttachmentViewTest::TearDown();
  }
  /** A received picture of `mime` holding `bytes` (padded to `size`), stored encrypted: its message. */
  ThreadMessage Stored(const std::string& id, std::vector<uint8_t> bytes, const std::string& mime = "image/png",
                       const size_t size = 0) {
    if (bytes.size() < size) {
      bytes.resize(size, static_cast<uint8_t>(id.size()));
    }
    bytes.push_back(static_cast<uint8_t>(id.back()));
    auto hash = AttachmentContentHash(bytes);
    EXPECT_TRUE(hash);
    EXPECT_TRUE(SaveAttachmentPlaintext(*content_, *hash, mime, bytes, "pic", TestDek()));
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/pic";
    fields.mime = mime;
    fields.filename = mime == "image/gif" ? "pic.gif" : "pic.png";
    fields.byte_length = bytes.size();
    fields.content_hash = *hash;
    fields.content_key.assign(kSessionKeySize, 1);
    fields.blob_nonce.assign(kAeadNonceSize, 2);
    hashes_[id] = *hash;
    ThreadMessage message;
    message.id = id;
    message.thread_id = "t1";
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return message;
  }
  TranscriptAttachmentView Draw(const ThreadMessage& message) { return inbox_->Facts().attachment(message); }
  std::string PreviewOf(const std::string& id) const {
    return AttachmentPosterPath(data_dir_.string(), "t1", hashes_.at(id));
  }
  bool Settled(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      AppRuntime::RunUITasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
  }
  static std::vector<uint8_t> GifHeader(const uint16_t w, const uint16_t h) {
    return {'G', 'I', 'F', '8', '9', 'a', static_cast<uint8_t>(w), static_cast<uint8_t>(w >> 8),
            static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8), 0, 0, 0};
  }

  std::shared_ptr<CasStore> content_;
  std::unique_ptr<AttachmentFetchWorkflow> downloads_;
  std::atomic<int> calls_{0};
  std::atomic<int> made_{0};
  std::atomic<int> failed_{0};
  std::atomic<bool> fail_{false};
  std::map<std::string, std::vector<uint8_t>> hashes_;
};

TEST_F(InboxPicturePreviewTest, APictureLongerThanThePreviewShowsItsPreviewWithItsOwnSize) {
  const ThreadMessage message = Stored("m1", PngHeader(1000, 800));
  ASSERT_TRUE(Draw(message).picture_placeholder);
  ASSERT_TRUE(Settled([&] { return made_.load() == 1; }));
  const auto view = Draw(message);
  EXPECT_EQ(view.local_path, PreviewOf("m1"));
  EXPECT_FALSE(view.picture_placeholder);
  EXPECT_TRUE(view.inline_image_ok);
  EXPECT_EQ(view.image_width, 1000) << "laid out as the placeholder was: nothing jumps";
  EXPECT_EQ(view.image_height, 800);
}

TEST_F(InboxPicturePreviewTest, WithoutItsPreviewItIsAPlaceholderOfItsSizeAndThePreviewIsAskedForOnce) {
  const ThreadMessage message = Stored("m1", PngHeader(1000, 800));
  const auto view = Draw(message);
  EXPECT_TRUE(view.picture_placeholder);
  EXPECT_TRUE(view.local_path.empty()) << "the full picture never goes into the bubble";
  EXPECT_EQ(view.state, AttachmentFetchWorkflow::DownloadState::Ready);
  EXPECT_EQ(view.image_width, 1000);
  EXPECT_EQ(view.image_height, 800);
  (void)Draw(message); // drawn again while it is being made
  ASSERT_TRUE(Settled([&] { return made_.load() == 1; }));
  EXPECT_EQ(calls_.load(), 1);
}

TEST_F(InboxPicturePreviewTest, APictureNoLongerThanThePreviewShowsItselfAndAsksForNothing) {
  const ThreadMessage message = Stored("m1", PngHeader(640, 300));
  const auto view = Draw(message);
  EXPECT_FALSE(view.picture_placeholder);
  EXPECT_FALSE(view.local_path.empty());
  EXPECT_EQ(view.local_path.find(".poster.jpg"), std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(calls_.load(), 0);
}

TEST_F(InboxPicturePreviewTest, APreviewThatCannotBeMadeLeavesAFileCardAndIsNotAskedAgain) {
  fail_ = true;
  const ThreadMessage message = Stored("m1", PngHeader(1000, 800));
  ASSERT_TRUE(Draw(message).picture_placeholder);
  ASSERT_TRUE(Settled([&] { return failed_.load() == 1; }));
  const auto view = Draw(message);
  EXPECT_FALSE(view.picture_placeholder);
  EXPECT_FALSE(view.inline_image_ok) << "a file card";
  EXPECT_FALSE(view.local_path.empty()) << "that still opens the picture";
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(calls_.load(), 1);
}

TEST_F(InboxPicturePreviewTest, AnUnreadableSizeIsAFileCard) {
  const ThreadMessage message = Stored("m1", {'n', 'o', 't', ' ', 'a', ' ', 'p', 'i', 'c'});
  const auto view = Draw(message);
  EXPECT_FALSE(view.inline_image_ok);
  EXPECT_FALSE(view.picture_placeholder);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(calls_.load(), 0);
}

TEST_F(InboxPicturePreviewTest, AGifIsTreatedLikeAnyPicture) {
  const ThreadMessage big = Stored("g1", GifHeader(1000, 800), "image/gif");
  const ThreadMessage small = Stored("g2", GifHeader(200, 200), "image/gif");
  EXPECT_TRUE(Draw(big).picture_placeholder);
  EXPECT_FALSE(Draw(small).picture_placeholder);
  EXPECT_FALSE(Draw(small).local_path.empty());
  ASSERT_TRUE(Settled([&] { return made_.load() == 1; }));
  EXPECT_EQ(Draw(big).local_path, PreviewOf("g1"));
}

TEST_F(InboxPicturePreviewTest, ALargePictureIsNotDecryptedToDrawItsCard) {
  const size_t large = static_cast<size_t>(kMaxDirectAttachmentBytes) + 100;
  const ThreadMessage message = Stored("big", PngHeader(4000, 3000), "image/png", large);
  const auto first = Draw(message);
  EXPECT_TRUE(first.picture_placeholder);
  EXPECT_EQ(first.image_width, kPicturePlaceholderFallbackWidth);
  EXPECT_EQ(first.image_height, kPicturePlaceholderFallbackHeight);
  ASSERT_TRUE(Settled([&] { return made_.load() == 1; }));
  const auto second = Draw(message);
  EXPECT_EQ(second.local_path, PreviewOf("big"));
  EXPECT_EQ(second.image_width, 640) << "its size is the preview's";
  EXPECT_EQ(second.image_height, 512);
  // No plaintext view of the large file was written to draw it: only the preview is in the view cache.
  size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(AttachmentViewRoot(data_dir_.string(), "t1"))) {
    (void)entry;
    ++files;
  }
  EXPECT_EQ(files, 1u);
}

// Stopped, not failed: a lock (the view cache wiped), leaving the chat (cancelled) or the cache wiped after it was
// made: the next drawing asks again, and the row stays a picture.
TEST_F(InboxPicturePreviewTest, AnInterruptedPreviewIsAskedForAgainAndNeverTurnsIntoAFileCard) {
  const ThreadMessage message = Stored("m1", PngHeader(1000, 800));
  downloads_->ClearDek(); // locked: it waits
  (void)Draw(message);
  downloads_->CancelChatPreviews(); // the chat left before the unlock
  ASSERT_TRUE(downloads_->SetDek(TestDek()));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(calls_.load(), 0);
  EXPECT_TRUE(Draw(message).picture_placeholder) << "drawn again: asked again";
  ASSERT_TRUE(Settled([&] { return made_.load() == 1; }));
  ASSERT_EQ(Draw(message).local_path, PreviewOf("m1"));
  ASSERT_TRUE(WipeAllAttachmentViewCaches(data_dir_.string())); // a lock's wipe after it was made
  const auto again = Draw(message);
  EXPECT_TRUE(again.picture_placeholder);
  EXPECT_TRUE(again.inline_image_ok);
  ASSERT_TRUE(Settled([&] { return made_.load() == 2; }));
  EXPECT_EQ(failed_.load(), 0);
}

// No drawing may name a file that is not there (the UI library remembers a failed load for the whole run): a video's
// poster only when its file is there; and under the lock nothing of the view cache at all, even a file the lock's
// wipe has not reached yet (the DEK goes first, the files after).
TEST_F(InboxPicturePreviewTest, NoFileIsNamedThatIsNotThereOrUnderTheLock) {
  const ThreadMessage picture = Stored("m1", PngHeader(1000, 800));
  const ThreadMessage small = Stored("m2", PngHeader(600, 400));
  const ThreadMessage video = Stored("v3", {'v', 'i', 'd', 'e', 'o'}, "video/mp4");
  ASSERT_TRUE(Draw(picture).picture_placeholder);
  ASSERT_TRUE(Settled([&] { return made_.load() == 1; }));
  ASSERT_EQ(Draw(picture).local_path, PreviewOf("m1"));
  const std::string small_view = Draw(small).local_path;
  ASSERT_FALSE(small_view.empty());

  EXPECT_TRUE(Draw(video).poster_path.empty()) << "no poster file: no poster";
  const auto put = [](const std::string& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  };
  put(PreviewOf("v3"), PngHeader(640, 360));
  EXPECT_EQ(Draw(video).poster_path, PreviewOf("v3")) << "its file is there";

  downloads_->ClearDek();
  // The drawing raced the wipe: the files are still there.
  put(PreviewOf("m1"), PngHeader(640, 512));
  put(small_view, PngHeader(600, 400));
  put(PreviewOf("v3"), PngHeader(640, 360));
  for (const ThreadMessage* message : {&picture, &small}) {
    const auto locked = Draw(*message);
    EXPECT_TRUE(locked.local_path.empty()) << message->id << ": neither its preview nor its own view";
    EXPECT_TRUE(locked.picture_placeholder) << message->id << ": its placeholder";
    EXPECT_GT(locked.image_width, 0);
  }
  EXPECT_TRUE(Draw(video).poster_path.empty());

  ASSERT_TRUE(downloads_->SetDek(TestDek())); // unlocked: shown again
  EXPECT_EQ(Draw(picture).local_path, PreviewOf("m1"));
  EXPECT_EQ(Draw(video).poster_path, PreviewOf("v3"));
}

} // namespace
} // namespace pbr
