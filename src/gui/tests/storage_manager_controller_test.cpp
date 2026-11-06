// Me → Storage → Manage storage pages (DEV/PLAN-storage-manager.md ②③) with fake ports: the back steps per page,
// select mode, thumbnails only for the cells on the page, the delete confirmation and lock, and coalesced counts.
#include "gui/StorageManagerController.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/voice/AudioFilePlayer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

std::vector<uint8_t> Hash(const int n) {
  std::vector<uint8_t> hash(32, 0);
  hash[0] = static_cast<uint8_t>(n & 0xff);
  hash[1] = static_cast<uint8_t>((n >> 8) & 0xff);
  return hash;
}

pbr::StoredAttachmentItem Item(const std::string& thread, const int hash, const std::string& mime,
                               const uint64_t bytes, const bool server = true) {
  pbr::StoredAttachmentItem item;
  item.thread_id = thread;
  item.message_id = thread + "-" + std::to_string(hash);
  item.content_hash = Hash(hash);
  item.mime = mime;
  item.filename = "file" + std::to_string(hash) + ".pdf";
  item.bytes = bytes;
  item.timestamp_ms = 1000 + hash;
  item.has_server_copy = server;
  return item;
}

class StorageManagerControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    pbr::LocalizationService::Instance().SetPreferredLanguage("zh-Hans");

    pbr::StorageManagerPorts& ports = commands_.storage;
    ports.list_stored = [this](std::function<void(std::vector<pbr::StoredAttachmentItem>)> done) {
      ++list_calls_;
      done(items_);
    };
    ports.message_store_bytes = [](std::function<void(uint64_t)> done) { done(10 * kMiB); };
    ports.profile_bytes = [](std::function<void(uint64_t)> done) { done(500 * kMiB); };
    ports.releasable_bytes = [this](std::vector<pbr::StoredAttachmentRef> refs, std::function<void(uint64_t)> done) {
      last_releasable_refs_ = std::move(refs);
      done(releasable_);
    };
    ports.delete_local = [this](std::vector<pbr::StoredAttachmentRef> refs,
                                std::function<void(pbr::StoredAttachmentDeleteResult)> done) {
      deleted_refs_ = std::move(refs);
      delete_done_ = std::move(done);
    };
    ports.chat_names = []() {
      return std::vector<pbr::StorageChatName>{{.thread_id = "a", .name = "张三", .avatar_seed = "acct-a"},
                                               {.thread_id = "b", .name = "Leo", .avatar_seed = "acct-b"}};
    };
    ports.request_thumbnail = [this](const std::string&, const std::vector<uint8_t>&, const std::string&,
                                     const uint64_t generation) { thumbnail_requests_.push_back(generation); };
    ports.cancel_thumbnails = [this](const uint64_t generation) { cancelled_.push_back(generation); };
    ports.thumbnail_path = [this](const std::string& thread_id, const std::vector<uint8_t>& hash) {
      const auto found = thumbnails_.find(thread_id + "|" + std::to_string(hash[0]));
      return found != thumbnails_.end() ? found->second : std::string();
    };
    ports.set_on_changed = [this](std::function<void()> on_changed) { on_changed_ = std::move(on_changed); };
    ports.set_on_thumbnail_ready = [this](auto on_ready) { on_thumbnail_ready_ = std::move(on_ready); };
    ports.run_later = [this](std::chrono::milliseconds, std::function<void()> task) {
      later_.push_back(std::move(task));
    };

    navigation_.push_local_back = [this](const std::string& id, std::function<void()> commit) {
      ClearStep(id);
      back_stack_.emplace_back(id, std::move(commit));
    };
    navigation_.clear_local_back = [this](const std::string& id) { ClearStep(id); };
    navigation_.has_local_back = [this](const std::string& id) {
      return std::any_of(back_stack_.begin(), back_stack_.end(), [&id](const auto& step) { return step.first == id; });
    };

    feedback_.show_confirm = [this](const std::string& title, const std::string& message,
                                    std::function<void(bool)> on_result, const std::string& ok_label) {
      confirm_title_ = title;
      confirm_message_ = message;
      confirm_ok_label_ = ok_label;
      confirm_ = std::move(on_result);
    };
  }

  void ClearStep(const std::string& id) {
    back_stack_.erase(std::remove_if(back_stack_.begin(), back_stack_.end(),
                                     [&id](const auto& step) { return step.first == id; }),
                      back_stack_.end());
  }

  /** The phone's back (header button, swipe, system back): the shell pops the top step and runs it. */
  bool SystemBack() {
    if (back_stack_.empty()) {
      return false;
    }
    auto step = std::move(back_stack_.back());
    back_stack_.pop_back();
    step.second();
    return true;
  }

  std::vector<pbr::StoredAttachmentItem> items_;
  int list_calls_ = 0;
  uint64_t releasable_ = 0;
  std::vector<pbr::StoredAttachmentRef> last_releasable_refs_;
  std::vector<pbr::StoredAttachmentRef> deleted_refs_;
  std::function<void(pbr::StoredAttachmentDeleteResult)> delete_done_;
  std::vector<uint64_t> thumbnail_requests_;
  std::vector<uint64_t> cancelled_;
  std::function<void()> on_changed_;
  std::function<void(const std::string&, const std::vector<uint8_t>&)> on_thumbnail_ready_;
  std::map<std::string, std::string> thumbnails_; // "thread|first hash byte" → path
  std::vector<std::function<void()>> later_;
  std::vector<std::pair<std::string, std::function<void()>>> back_stack_;
  std::string confirm_title_;
  std::string confirm_message_;
  std::string confirm_ok_label_;
  std::function<void(bool)> confirm_;
  bool visible_ = true;

  pbr::SettingsCommands commands_;
  pbr::ShellNavigationPorts navigation_;
  pbr::ShellFeedbackPorts feedback_;
  pbr::StorageManagerController controller_{commands_, navigation_, feedback_, [this]() { return visible_; }};
};

} // namespace

TEST_F(StorageManagerControllerTest, ChatsAreListedBySizeWithTheirNames) {
  items_ = {Item("a", 1, "image/jpeg", 2 * kMiB), Item("b", 2, "video/mp4", 30 * kMiB),
            Item("b", 3, "application/pdf", 1 * kMiB)};
  controller_.Enter();
  ASSERT_EQ(controller_.Chats().size(), 2u);
  EXPECT_EQ(std::string(controller_.Chats()[0].name.c_str()), "Leo");
  EXPECT_EQ(std::string(controller_.Chats()[0].summary.c_str()), "视频 1 · 文件 1");
  EXPECT_EQ(std::string(controller_.Chats()[1].name.c_str()), "张三");
  EXPECT_EQ(std::string(controller_.Chats()[1].summary.c_str()), "图片 1");
}

TEST_F(StorageManagerControllerTest, EnteringStorageGoesBackToTheFirstPageAndDropsTheSelection) {
  items_ = {Item("a", 1, "image/jpeg", 2 * kMiB), Item("a", 2, "image/jpeg", 3 * kMiB)};
  controller_.Enter();
  EXPECT_EQ(list_calls_, 1);
  controller_.OpenManage();
  controller_.OpenChat(0);
  controller_.ToggleSelect();
  controller_.TapCell(0);
  ASSERT_EQ(controller_.Page(), "items");
  ASSERT_TRUE(controller_.Selecting());
  ASSERT_EQ(controller_.SelectedCount(), 1u);
  ASSERT_EQ(back_stack_.size(), 3u);

  controller_.Enter();
  EXPECT_EQ(controller_.Page(), "main");
  EXPECT_FALSE(controller_.Selecting());
  EXPECT_EQ(controller_.SelectedCount(), 0u);
  EXPECT_TRUE(back_stack_.empty());
  EXPECT_EQ(list_calls_, 2) << "counted again on entering";
}

TEST_F(StorageManagerControllerTest, BackWhileSelectingLeavesSelectModeFirst) {
  items_ = {Item("a", 1, "image/jpeg", 2 * kMiB)};
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  controller_.PickByLongPress(0);  // a long press starts selecting with that cell picked
  ASSERT_TRUE(controller_.Selecting());
  ASSERT_EQ(controller_.SelectedCount(), 1u);
  EXPECT_TRUE(controller_.Cells()[0].picked);

  // Phone: the system back goes one step at a time.
  ASSERT_TRUE(SystemBack());
  EXPECT_EQ(controller_.Page(), "items");
  EXPECT_FALSE(controller_.Selecting());
  EXPECT_FALSE(controller_.Cells()[0].picked);
  ASSERT_TRUE(SystemBack());
  EXPECT_EQ(controller_.Page(), "manage");
  ASSERT_TRUE(SystemBack());
  EXPECT_EQ(controller_.Page(), "main");
  EXPECT_FALSE(SystemBack());

  // Desktop: the page's own back button, the same order, and the shell's steps go with it.
  controller_.OpenManage();
  controller_.OpenChat(0);
  controller_.ToggleSelect();
  controller_.Back();
  EXPECT_EQ(controller_.Page(), "items");
  EXPECT_FALSE(controller_.Selecting());
  EXPECT_EQ(back_stack_.size(), 2u);
  controller_.Back();
  EXPECT_EQ(controller_.Page(), "manage");
  controller_.Back();
  EXPECT_EQ(controller_.Page(), "main");
  EXPECT_TRUE(back_stack_.empty());
}

TEST_F(StorageManagerControllerTest, LayoutSwitchKeepsThePageAndPutsItsBackStepsBack) {
  items_ = {Item("a", 1, "image/jpeg", 2 * kMiB)};
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  controller_.ToggleSelect();
  back_stack_.clear();  // the shell dropped its stack when the layout changed
  controller_.RestoreBackStack();
  EXPECT_EQ(controller_.Page(), "items");
  ASSERT_EQ(back_stack_.size(), 3u);
  EXPECT_EQ(back_stack_[0].first, "storage_manage");
  EXPECT_EQ(back_stack_[2].first, "storage_select");
}

TEST_F(StorageManagerControllerTest, ThumbnailsOnlyForTheCellsOnThePage) {
  for (int i = 0; i < 70; ++i) {
    items_.push_back(Item("a", i, "image/jpeg", kMiB));
  }
  items_.push_back(Item("a", 500, "video/mp4", 8 * kMiB));
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  EXPECT_EQ(controller_.Tab(), "all");
  EXPECT_EQ(controller_.Cells().size(), pbr::kStoragePageSize);
  ASSERT_EQ(thumbnail_requests_.size(), pbr::kStoragePageSize);
  const uint64_t generation = thumbnail_requests_.front();

  controller_.LoadMore();
  EXPECT_EQ(controller_.Cells().size(), 71u) << "the video sits in the grid with the pictures";
  EXPECT_EQ(thumbnail_requests_.size(), 71u) << "the next batch only, nothing asked twice";

  controller_.SetTab("video");
  EXPECT_NE(std::find(cancelled_.begin(), cancelled_.end(), generation), cancelled_.end())
      << "switching tab drops the old page's jobs";
  EXPECT_EQ(controller_.Cells().size(), 1u);
  EXPECT_TRUE(controller_.Cells()[0].video);
  const uint64_t video_generation = thumbnail_requests_.back();
  EXPECT_NE(video_generation, generation);

  controller_.Back();
  EXPECT_NE(std::find(cancelled_.begin(), cancelled_.end(), video_generation), cancelled_.end())
      << "leaving the page drops its jobs";
}

TEST_F(StorageManagerControllerTest, ChatPageListsPicturesVideosAndFilesTogether) {
  items_ = {Item("a", 1, "image/jpeg", 2 * kMiB), Item("a", 2, "video/mp4", 8 * kMiB),
            Item("a", 3, "application/pdf", 3 * kMiB), Item("a", 4, "application/zip", 512 * 1024)};
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  EXPECT_EQ(controller_.Tab(), "all");
  ASSERT_EQ(controller_.Cells().size(), 2u);
  EXPECT_TRUE(controller_.Cells()[0].video) << "newest first: the video";
  EXPECT_FALSE(controller_.Cells()[1].video);
  EXPECT_EQ(std::string(controller_.Cells()[0].size.c_str()), "8.0 MB");
  ASSERT_EQ(controller_.Files().size(), 2u);
  EXPECT_EQ(std::string(controller_.Files()[0].name.c_str()), "file4.pdf");
  EXPECT_EQ(std::string(controller_.Files()[0].size.c_str()), "512 KB");
  EXPECT_EQ(std::string(controller_.Files()[1].size.c_str()), "3.0 MB");

  // Selecting a file row, a video cell and a picture works the same; the delete weighs all three.
  controller_.ToggleSelect();
  controller_.TapFile(1);
  controller_.TapCell(0);
  EXPECT_TRUE(controller_.Files()[1].picked);
  EXPECT_FALSE(controller_.Files()[0].picked);
  EXPECT_TRUE(controller_.Cells()[0].picked);
  EXPECT_EQ(controller_.SelectedCount(), 2u);
  controller_.TapFile(1);
  EXPECT_EQ(controller_.SelectedCount(), 1u);
  controller_.TapFile(0);
  controller_.DeleteSelected();
  ASSERT_TRUE(confirm_);
  confirm_(true);
  ASSERT_EQ(deleted_refs_.size(), 2u);
  ASSERT_TRUE(delete_done_);
  delete_done_({.deleted = 2, .freed_bytes = 0, .kept_shared = 0});

  // A long press on a file row (index after the cells) starts selecting with that row picked.
  ASSERT_FALSE(controller_.Selecting());
  controller_.PickByLongPress(2);
  ASSERT_EQ(controller_.Files().size(), 2u);
  EXPECT_TRUE(controller_.Selecting());
  EXPECT_TRUE(controller_.Files()[0].picked);

  controller_.SetTab("file");
  EXPECT_TRUE(controller_.Cells().empty());
  EXPECT_EQ(controller_.Files().size(), 2u);
}

namespace {

/** The system's sound-file player as the storage page test drives it. */
struct PageAudio {
  std::vector<std::string> played;
  std::vector<std::string> calls;
  pbr::AudioFilePlayerCallbacks callbacks;
};

class FakePagePlayer final : public pbr::IAudioFilePlayer {
public:
  explicit FakePagePlayer(std::shared_ptr<PageAudio> audio) : audio_(std::move(audio)) {}
  bool Play(const std::string& path, pbr::AudioFilePlayerCallbacks callbacks) override {
    audio_->played.push_back(path);
    audio_->callbacks = std::move(callbacks);
    return true;
  }
  void Pause() override { audio_->calls.push_back("pause"); }
  void Resume() override { audio_->calls.push_back("resume"); }
  void Stop() override { audio_->calls.push_back("stop"); }

private:
  std::shared_ptr<PageAudio> audio_;
};

} // namespace

// #111: a sound file's row plays it on the page (not in the system's viewer): play / pause in the badge, the time
// under the name and a bar; a tap pauses; leaving the page stops it and hands the session back.
TEST_F(StorageManagerControllerTest, ASoundFileRowPlaysInPlace) {
  auto audio = std::make_shared<PageAudio>();
  std::vector<bool> session;
  commands_.storage.make_audio_player = [audio]() { return std::make_unique<FakePagePlayer>(audio); };
  commands_.storage.audio_session = [&session](const bool playing) { session.push_back(playing); };
  std::vector<std::string> opened;
  commands_.storage.open_item = [&opened](const pbr::StoredAttachmentItem& item,
                                          std::function<void(std::string, bool)> done) {
    opened.push_back(item.filename);
    done("/views/" + item.filename, true);
  };
  pbr::StoredAttachmentItem song = Item("a", 5, "audio/mpeg", 4 * kMiB);
  song.filename = "song.mp3";
  items_ = {Item("a", 3, "application/pdf", 3 * kMiB), song};
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  ASSERT_EQ(controller_.Files().size(), 2u);
  const auto song_row = [this]() -> const pbr::StorageManagerController::FileRow& {
    return std::string(controller_.Files()[0].name.c_str()) == "song.mp3" ? controller_.Files()[0]
                                                                          : controller_.Files()[1];
  };
  const int song_index = std::string(controller_.Files()[0].name.c_str()) == "song.mp3" ? 0 : 1;
  EXPECT_TRUE(song_row().audio);
  EXPECT_FALSE(controller_.Files()[1 - song_index].audio) << "a PDF still opens in the system's viewer";

  controller_.TapFile(song_index);
  ASSERT_EQ(audio->played, std::vector<std::string>{"/views/song.mp3"});
  EXPECT_TRUE(confirm_ == nullptr) << "a sound plays here: no 'open in another app' question";
  EXPECT_TRUE(song_row().playing);
  EXPECT_EQ(session, std::vector<bool>{true});

  audio->callbacks.on_progress(60000, 240000);
  EXPECT_EQ(std::string(song_row().progress.c_str()), "25%");
  EXPECT_EQ(std::string(song_row().detail.c_str()), "1:00 / 4:00");

  controller_.TapFile(song_index); // pause
  EXPECT_FALSE(song_row().playing);
  EXPECT_EQ(std::string(song_row().progress.c_str()), "25%");
  controller_.TapFile(song_index); // resume
  EXPECT_TRUE(song_row().playing);
  EXPECT_EQ(audio->calls, (std::vector<std::string>{"pause", "resume"}));

  ASSERT_TRUE(SystemBack()); // leave the chat's page
  EXPECT_EQ(audio->calls.back(), "stop");
  EXPECT_EQ(session, (std::vector<bool>{true, false}));
}

TEST_F(StorageManagerControllerTest, AllTabListsEveryFileOnTheFirstPageOfManyPictures) {
  for (int i = 0; i < 70; ++i) {
    items_.push_back(Item("a", i, "image/jpeg", kMiB));
  }
  items_.push_back(Item("a", 900, "application/pdf", kMiB));
  items_.push_back(Item("a", 901, "application/zip", kMiB));
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  EXPECT_EQ(controller_.Cells().size(), pbr::kStoragePageSize);
  EXPECT_EQ(controller_.Files().size(), 2u);
  controller_.ToggleSelect();
  controller_.TapFile(1);
  EXPECT_TRUE(controller_.Files()[1].picked);
  EXPECT_EQ(controller_.SelectedCount(), 1u);
  controller_.LoadMore();
  EXPECT_EQ(controller_.Cells().size(), 70u);
  EXPECT_EQ(controller_.Files().size(), 2u);
}

TEST_F(StorageManagerControllerTest, DeleteAsksOnceThenLocksThePageUntilDone) {
  items_ = {
      Item("a", 1, "image/jpeg", 3 * kMiB, /*server=*/true),
      Item("b", 1, "image/jpeg", 3 * kMiB, /*server=*/true),  // also held by chat b
      Item("a", 2, "image/jpeg", 4 * kMiB, /*server=*/false), // only on this device
  };
  releasable_ = 4 * kMiB;
  controller_.Enter();
  controller_.OpenManage();
  // "a" holds 7 MB, "b" 3 MB: a comes first.
  controller_.OpenChat(0);
  controller_.ToggleSelect();
  controller_.TapCell(0);
  controller_.TapCell(1);
  EXPECT_EQ(controller_.SelectionLabel(), "已选 2 项 · 可释放 4.0 MB");

  controller_.DeleteSelected();
  ASSERT_TRUE(confirm_);
  EXPECT_EQ(confirm_title_, "从这台设备删除 2 项（可释放 4.0 MB）？");
  EXPECT_NE(confirm_message_.find("服务器上过期了就下不回来"), std::string::npos) << confirm_message_;
  EXPECT_NE(confirm_message_.find("其中 1 项没有存在服务器上，删了就找不回来。"), std::string::npos)
      << confirm_message_;
  EXPECT_NE(confirm_message_.find("其中 1 项别的聊天也在用，这次不会释放空间。"), std::string::npos)
      << confirm_message_;
  EXPECT_EQ(confirm_ok_label_, "删除");
  EXPECT_TRUE(deleted_refs_.empty()) << "nothing before the confirmation";

  confirm_(true);
  ASSERT_EQ(deleted_refs_.size(), 2u);
  EXPECT_EQ(deleted_refs_[0].first, "a");
  EXPECT_EQ(deleted_refs_[1].first, "a");
  EXPECT_TRUE(controller_.Busy());
  controller_.TapCell(0);  // locked: no toggle while deleting
  EXPECT_EQ(controller_.SelectedCount(), 2u);

  const int lists_before = list_calls_;
  items_ = {Item("b", 1, "image/jpeg", 3 * kMiB)};
  ASSERT_TRUE(delete_done_);
  delete_done_({.deleted = 2, .freed_bytes = 4 * kMiB, .kept_shared = 1});
  EXPECT_FALSE(controller_.Busy());
  EXPECT_FALSE(controller_.Selecting());
  EXPECT_EQ(list_calls_, lists_before + 1) << "rebuilt from fresh data";
  EXPECT_TRUE(controller_.Cells().empty());
}

TEST_F(StorageManagerControllerTest, DeletingFromTheLargeListNamesEveryChat) {
  items_ = {Item("a", 7, "video/mp4", 40 * kMiB), Item("b", 7, "video/mp4", 40 * kMiB)};
  releasable_ = 40 * kMiB;
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenLarge();
  ASSERT_EQ(controller_.Page(), "items");
  EXPECT_EQ(controller_.Tab(), "all");
  ASSERT_EQ(controller_.Cells().size(), 1u);
  controller_.ToggleSelect();
  controller_.TapCell(0);
  controller_.DeleteSelected();
  ASSERT_TRUE(confirm_);
  EXPECT_EQ(confirm_message_.find("别的聊天也在用"), std::string::npos) << confirm_message_;
  confirm_(true);
  ASSERT_EQ(deleted_refs_.size(), 2u);
  EXPECT_EQ(deleted_refs_[0].first, "a");
  EXPECT_EQ(deleted_refs_[1].first, "b");
}

TEST_F(StorageManagerControllerTest, ChangesAreCountedAtMostOnceAWhileAndKeepTheSelection) {
  items_ = {Item("a", 1, "image/jpeg", 2 * kMiB), Item("a", 2, "image/jpeg", 2 * kMiB)};
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  controller_.ToggleSelect();
  controller_.TapCell(1);
  ASSERT_TRUE(on_changed_);
  ASSERT_EQ(list_calls_, 1);

  for (int i = 0; i < 20; ++i) {
    on_changed_();
  }
  EXPECT_LE(list_calls_, 2);
  EXPECT_LE(later_.size(), 1u) << "one pending count at most";
  while (!later_.empty()) {
    auto task = std::move(later_.front());
    later_.erase(later_.begin());
    task();
  }
  EXPECT_LE(list_calls_, 2);
  EXPECT_GE(list_calls_, 2) << "the burst is counted once";
  EXPECT_EQ(controller_.Page(), "items");
  EXPECT_TRUE(controller_.Selecting());
  EXPECT_EQ(controller_.SelectedCount(), 1u) << "what is still there stays selected";

  // Not shown (another section, Me closed): nothing is counted.
  visible_ = false;
  const int before = list_calls_;
  on_changed_();
  EXPECT_EQ(list_calls_, before);
}

TEST_F(StorageManagerControllerTest, PickingWeighsTheSelectionFromTheLoadedListWithoutAskingTheDevice) {
  items_ = {Item("a", 1, "image/jpeg", 3 * kMiB), Item("b", 1, "image/jpeg", 3 * kMiB),
            Item("a", 2, "image/jpeg", 4 * kMiB)};
  bool asked = false;
  commands_.storage.releasable_bytes = [&asked](std::vector<pbr::StoredAttachmentRef>,
                                                std::function<void(uint64_t)> done) {
    asked = true;
    done(0);
  };
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  controller_.ToggleSelect();
  for (int i = 0; i < 48; ++i) {
    controller_.TapCell(i % 2); // each picked and unpicked 12 times
  }
  controller_.TapCell(0);
  controller_.TapCell(1);
  EXPECT_FALSE(asked) << "no scan of every chat per tap";
  EXPECT_EQ(controller_.SelectionLabel(), "已选 2 项 · 可释放 4.0 MB") << "the shared one frees nothing";
  controller_.TapCell(1); // the shared one off: the 4 MB only here stays
  EXPECT_EQ(controller_.SelectionLabel(), "已选 1 项 · 可释放 4.0 MB");
}

TEST_F(StorageManagerControllerTest, AThumbnailMadeRefreshesItsCellOnlyWithoutACount) {
  items_ = {Item("a", 1, "image/jpeg", kMiB), Item("a", 2, "image/jpeg", kMiB)};
  controller_.Enter();
  controller_.OpenManage();
  controller_.OpenChat(0);
  ASSERT_EQ(controller_.Cells().size(), 2u);
  ASSERT_TRUE(on_thumbnail_ready_);
  const int lists_before = list_calls_;

  thumbnails_["a|2"] = "/thumbs/2.jpg";
  on_thumbnail_ready_("a", Hash(2));

  EXPECT_EQ(list_calls_, lists_before) << "nothing is counted again";
  EXPECT_TRUE(later_.empty());
  // Newest first: hash 2 is the first cell.
  EXPECT_EQ(std::string(controller_.Cells()[0].thumb.c_str()), "/thumbs/2.jpg");
  EXPECT_TRUE(controller_.Cells()[1].thumb.empty());
}
