// Every received attachment has a way out (DEV/PLAN-received-file-save-as.md step 3): what a tap and a menu do on a
// computer and on a phone, and the desktop save flow with fake ports — the dialog answers on another thread, the copy
// runs on a worker, every state change and pill happens on the UI thread (the test's own thread here).
#include "feature/presenters/AttachmentExit.h"

#include "common/PathUtf8.h"
#include "common/Utilities.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/SavedFileCopy.h"

#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

constexpr AttachmentLayout kDesktop = AttachmentLayout::Desktop;
constexpr AttachmentLayout kPhone = AttachmentLayout::Phone;

TEST(AttachmentExitTest, KindsFollowTheTranscriptsRules) {
  EXPECT_EQ(KindOfAttachment("image/png", "a.png", 10, 120, true), AttachmentExitKind::Picture);
  EXPECT_EQ(KindOfAttachment("video/mp4", "a.mp4", 10, 120, true), AttachmentExitKind::Video);
  EXPECT_EQ(KindOfAttachment("audio/ogg", "voice-4200.ogg", 5000, 120, true), AttachmentExitKind::Voice);
  EXPECT_EQ(KindOfAttachment("audio/ogg", "voice-4200.ogg", 5000, 120, false), AttachmentExitKind::AudioFile)
      << "no voice bubbles on this device: the card is a sound file";
  EXPECT_EQ(KindOfAttachment("audio/mpeg", "song.mp3", 5'000'000, 120, true), AttachmentExitKind::AudioFile);
  EXPECT_EQ(KindOfAttachment("application/octet-stream", "song.mp3", 5'000'000, 120, true),
            AttachmentExitKind::AudioFile)
      << "the sender's name types a sound file sent as octet-stream";
  EXPECT_EQ(KindOfAttachment("application/gzip", "pp-logs.gz", 30'000, 120, true), AttachmentExitKind::File);
}

TEST(AttachmentExitTest, ADesktopClickOnAFileSavesAndPicturesAndVideosOpenAsBefore) {
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::File, true, true, false), AttachmentTap::Save);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::File, false, true, false), AttachmentTap::NotReady);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::Picture, true, true, false), AttachmentTap::Open);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::Video, true, true, false), AttachmentTap::Open);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::Voice, true, true, false), AttachmentTap::Open);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::AudioFile, false, false, false), AttachmentTap::NotReady);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::AudioFile, true, false, false), AttachmentTap::Save)
      << "no in-app player on this computer: a sound file is a file";
}

TEST(AttachmentExitTest, AnOctetStreamMp3ClickedOnADesktopThatPlaysItOpensNoDialog) {
  const AttachmentExitKind kind = KindOfAttachment("application/octet-stream", "song.mp3", 5'000'000, 120, true);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, kind, true, /*plays_in_app=*/true, false), AttachmentTap::PlayInApp);
}

TEST(AttachmentExitTest, EveryPhoneTapIsAsBefore) {
  for (const AttachmentExitKind kind :
       {AttachmentExitKind::Picture, AttachmentExitKind::Video, AttachmentExitKind::AudioFile,
        AttachmentExitKind::Voice, AttachmentExitKind::File}) {
    for (const bool ready : {true, false}) {
      EXPECT_EQ(DecideAttachmentTap(kPhone, kind, ready, true, false), AttachmentTap::Open);
      EXPECT_EQ(DecideAttachmentTap(kPhone, kind, ready, false, true), AttachmentTap::Open);
    }
  }
}

TEST(AttachmentExitTest, WhileASaveIsBusyEveryDesktopTapIsIgnored) {
  for (const AttachmentExitKind kind :
       {AttachmentExitKind::Picture, AttachmentExitKind::Video, AttachmentExitKind::AudioFile,
        AttachmentExitKind::Voice, AttachmentExitKind::File}) {
    EXPECT_EQ(DecideAttachmentTap(kDesktop, kind, true, true, /*save_busy=*/true), AttachmentTap::Ignore);
  }
}

TEST(AttachmentExitTest, SaveAsIsOfferedOnTheDesktopForEveryAttachmentButAVoiceMessage) {
  for (const AttachmentExitKind kind : {AttachmentExitKind::Picture, AttachmentExitKind::Video,
                                        AttachmentExitKind::AudioFile, AttachmentExitKind::File}) {
    EXPECT_TRUE(DecideMenuEntries(kDesktop, kind).save_as);
    EXPECT_FALSE(DecideMenuEntries(kPhone, kind).save_as);
  }
  EXPECT_FALSE(DecideMenuEntries(kDesktop, AttachmentExitKind::Voice).save_as);
  EXPECT_FALSE(DecideMenuEntries(kDesktop, std::nullopt).save_as) << "a text message";
}

TEST(AttachmentExitTest, TheSuggestedNameIsTheSafeNameElseTheSendersNameCleaned) {
  EXPECT_EQ(SuggestedSaveName("../../evil/report.pdf", "report.pdf"), "report.pdf");
  EXPECT_EQ(SuggestedSaveName("../dir\\.data:1.json", ""), "data1.json");
  EXPECT_EQ(SuggestedSaveName("...", ""), "attachment");
  EXPECT_EQ(SuggestedSaveName("", ""), "attachment");
  EXPECT_EQ(SuggestedSaveName(std::string(150, 'a') + ".json", "").size(), 100u);
}

TEST(AttachmentExitTest, TheSaveStateGoesIdleChoosingSavingIdle) {
  AttachmentSaveState state;
  EXPECT_FALSE(state.Busy());
  EXPECT_FALSE(state.Chosen("m1")) << "nothing being chosen";
  ASSERT_TRUE(state.TryBegin("m1"));
  EXPECT_EQ(state.phase(), AttachmentSaveState::Phase::Choosing);
  EXPECT_FALSE(state.TryBegin("m2"));
  EXPECT_FALSE(state.Chosen("m2"));
  ASSERT_TRUE(state.Chosen("m1"));
  EXPECT_EQ(state.phase(), AttachmentSaveState::Phase::Saving);
  EXPECT_FALSE(state.TryBegin("m1"));
  state.Finish("m2");
  EXPECT_TRUE(state.Busy()) << "another message's finish leaves this one";
  state.Finish("m1");
  EXPECT_FALSE(state.Busy());
}

/** The app's side of the save flow: queues for the UI thread and the worker, a dialog answering on its own thread. */
class SaveFlowTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    dir_ = std::filesystem::temp_directory_path() / ("pp_attachment_exit_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_ / "views");
    std::filesystem::create_directories(dir_ / "Downloads");
    view_ = dir_ / "views" / "report.gz";
    content_ = std::string(kSavedFileChunkBytes + 12345, 'x');
    for (size_t i = 0; i < content_.size(); i += 997) {
      content_[i] = static_cast<char>(i & 0xFF);
    }
    std::ofstream(view_, std::ios::binary) << content_;
    ui_thread_ = std::this_thread::get_id();
  }
  void TearDown() override {
    if (dialog_.joinable()) {
      dialog_.join();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  AttachmentSavePorts Ports() {
    AttachmentSavePorts ports;
    ports.choose_path = [this](const std::string& name, std::function<void(SaveChoice)> done) {
      on_ui_.push_back(std::this_thread::get_id() == ui_thread_);
      ++dialogs_;
      offered_ = name;
      pending_dialog_ = std::move(done);
    };
    ports.post_ui = [this](std::function<void()> task) {
      std::lock_guard<std::mutex> lock(mu_);
      ui_tasks_.push_back(std::move(task));
    };
    ports.post_worker = [this](std::function<void()> task) {
      std::lock_guard<std::mutex> lock(mu_);
      worker_tasks_.push_back(std::move(task));
    };
    ports.view_path = [this](const std::string& thread_id,
                             const std::string& message_id) -> std::optional<std::string> {
      on_worker_.push_back(std::this_thread::get_id() != ui_thread_);
      asked_view_ = thread_id + "/" + message_id;
      if (!view_available_) {
        return std::nullopt;
      }
      return PathToUtf8(view_);
    };
    ports.copy = [this](const std::string& from, const std::string& to) {
      on_worker_.push_back(std::this_thread::get_id() != ui_thread_);
      ++copies_;
      return static_cast<bool>(CopyFileInChunks(from, to));
    };
    ports.mark_downloaded = [this](const std::string& path) {
      on_ui_.push_back(std::this_thread::get_id() == ui_thread_);
      marked_.push_back(path);
    };
    ports.folder_name = [](const std::string& folder) { return PathToUtf8(PathFromUtf8(folder).filename()); };
    ports.ok = [this](const std::string& text) {
      on_ui_.push_back(std::this_thread::get_id() == ui_thread_);
      oks_.push_back(text);
    };
    ports.fail = [this](const std::string& text) {
      on_ui_.push_back(std::this_thread::get_id() == ui_thread_);
      fails_.push_back(text);
    };
    return ports;
  }

  /** The dialog answers `choice` from another thread (as SDL may). */
  void Answer(SaveChoice choice) {
    ASSERT_TRUE(pending_dialog_);
    dialog_ = std::thread([done = std::exchange(pending_dialog_, {}), choice = std::move(choice)]() { done(choice); });
    dialog_.join();
  }
  /** Runs what was posted: UI tasks here, each worker task on a thread of its own. */
  void Pump() {
    for (;;) {
      std::function<void()> task;
      bool worker = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ui_tasks_.empty()) {
          task = std::move(ui_tasks_.front());
          ui_tasks_.pop_front();
        } else if (!worker_tasks_.empty()) {
          task = std::move(worker_tasks_.front());
          worker_tasks_.pop_front();
          worker = true;
        } else {
          return;
        }
      }
      if (worker) {
        std::thread(task).join();
      } else {
        task();
      }
    }
  }
  std::string Target() const { return PathToUtf8(dir_ / "Downloads" / "report.gz"); }
  static std::string Read(const std::string& path) {
    std::ifstream in(PathFromUtf8(path), std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }
  void ExpectAllOnTheirThreads() const {
    for (const bool ok : on_ui_) {
      EXPECT_TRUE(ok) << "a state change or pill off the UI thread";
    }
    for (const bool ok : on_worker_) {
      EXPECT_TRUE(ok) << "a decrypt or copy on the UI thread";
    }
  }

  std::filesystem::path dir_;
  std::filesystem::path view_;
  std::string content_;
  std::thread::id ui_thread_;
  std::thread dialog_;
  std::mutex mu_;
  std::deque<std::function<void()>> ui_tasks_;
  std::deque<std::function<void()>> worker_tasks_;
  std::function<void(SaveChoice)> pending_dialog_;
  bool view_available_ = true;
  int dialogs_ = 0;
  int copies_ = 0;
  std::string offered_;
  std::string asked_view_;
  std::vector<std::string> marked_;
  std::vector<std::string> oks_;
  std::vector<std::string> fails_;
  std::vector<bool> on_ui_;
  std::vector<bool> on_worker_;
};

TEST_F(SaveFlowTest, AChosenPathGetsTheWholeFileTheMarkAndTheSavedToPill) {
  AttachmentSaveFlow flow(Ports());
  ASSERT_EQ(flow.Begin("t1", "m1", "report.gz", true), AttachmentSaveFlow::Start::Started);
  EXPECT_EQ(offered_, "report.gz");
  EXPECT_TRUE(asked_view_.empty()) << "nothing decrypted before a path is chosen";
  Answer({SaveChoice::Kind::Chosen, Target()});
  EXPECT_TRUE(flow.Busy()) << "the answer waits for the UI thread";
  Pump();
  EXPECT_EQ(asked_view_, "t1/m1");
  EXPECT_EQ(Read(Target()), content_);
  EXPECT_EQ(marked_, std::vector<std::string>{Target()});
  EXPECT_EQ(oks_, (std::vector<std::string>{"正在存储…", "已存到 Downloads"}));
  EXPECT_TRUE(fails_.empty());
  EXPECT_FALSE(flow.Busy());
  ExpectAllOnTheirThreads();
}

TEST_F(SaveFlowTest, ACancelLeavesNoFileAndNoPill) {
  AttachmentSaveFlow flow(Ports());
  ASSERT_EQ(flow.Begin("t1", "m1", "report.gz", true), AttachmentSaveFlow::Start::Started);
  Answer({SaveChoice::Kind::Cancelled, {}});
  Pump();
  EXPECT_FALSE(std::filesystem::exists(PathFromUtf8(Target())));
  EXPECT_TRUE(oks_.empty());
  EXPECT_TRUE(fails_.empty());
  EXPECT_TRUE(asked_view_.empty());
  EXPECT_FALSE(flow.Busy());
  ExpectAllOnTheirThreads();
}

TEST_F(SaveFlowTest, NoDialogSaysItCannotSaveNow) {
  AttachmentSaveFlow flow(Ports());
  ASSERT_EQ(flow.Begin("t1", "m1", "report.gz", true), AttachmentSaveFlow::Start::Started);
  Answer({SaveChoice::Kind::Unavailable, {}});
  Pump();
  EXPECT_EQ(fails_, std::vector<std::string>{"现在无法存储"});
  EXPECT_TRUE(oks_.empty());
  EXPECT_FALSE(flow.Busy());
  ExpectAllOnTheirThreads();
}

TEST_F(SaveFlowTest, NoViewSaysItCouldNotSaveAndMakesNoTargetFile) {
  view_available_ = false; // the vault locked meanwhile, or the decrypt failed
  AttachmentSaveFlow flow(Ports());
  ASSERT_EQ(flow.Begin("t1", "m1", "report.gz", true), AttachmentSaveFlow::Start::Started);
  Answer({SaveChoice::Kind::Chosen, Target()});
  Pump();
  EXPECT_EQ(copies_, 0);
  EXPECT_FALSE(std::filesystem::exists(PathFromUtf8(Target())));
  EXPECT_EQ(fails_, std::vector<std::string>{"没能存储"});
  EXPECT_TRUE(marked_.empty());
  EXPECT_FALSE(flow.Busy());
  ExpectAllOnTheirThreads();
}

TEST_F(SaveFlowTest, AFailedCopyKeepsTheReplacedFileAndSaysSo) {
  std::ofstream(PathFromUtf8(Target()), std::ios::binary) << "the user's own file";
  AttachmentSavePorts ports = Ports();
  ports.copy = [](const std::string& from, const std::string& to) {
    return static_cast<bool>(CopyFileInChunks(from, to, [](const size_t chunk) { return chunk == 0; }));
  };
  AttachmentSaveFlow flow(std::move(ports));
  ASSERT_EQ(flow.Begin("t1", "m1", "report.gz", true), AttachmentSaveFlow::Start::Started);
  Answer({SaveChoice::Kind::Chosen, Target()});
  Pump();
  EXPECT_EQ(Read(Target()), "the user's own file");
  EXPECT_EQ(fails_, std::vector<std::string>{"没能存储"});
  EXPECT_TRUE(marked_.empty());
  EXPECT_FALSE(flow.Busy());
}

TEST_F(SaveFlowTest, NotDownloadedSaysSoAndOpensNoDialog) {
  AttachmentSaveFlow flow(Ports());
  EXPECT_EQ(flow.Begin("t1", "m1", "report.gz", false), AttachmentSaveFlow::Start::NotReady);
  EXPECT_EQ(dialogs_, 0);
  EXPECT_EQ(fails_, std::vector<std::string>{Tr("chat.attachment.not_ready")});
  EXPECT_FALSE(flow.Busy());
}

TEST_F(SaveFlowTest, ASecondSaveOrClickIsIgnoredWhileTheDialogIsUpOrTheCopyRuns) {
  AttachmentSaveFlow flow(Ports());
  ASSERT_EQ(flow.Begin("t1", "m1", "report.gz", true), AttachmentSaveFlow::Start::Started);
  EXPECT_EQ(flow.Begin("t1", "m1", "report.gz", true), AttachmentSaveFlow::Start::Ignored);
  EXPECT_EQ(flow.Begin("t1", "m2", "other.pdf", true), AttachmentSaveFlow::Start::Ignored);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::File, true, true, flow.Busy()), AttachmentTap::Ignore);
  EXPECT_EQ(dialogs_, 1);
  Answer({SaveChoice::Kind::Chosen, Target()});
  {
    // The answer is in; the UI task runs, the copy is queued but not run yet: still busy.
    std::function<void()> ui;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ui = std::move(ui_tasks_.front());
      ui_tasks_.pop_front();
    }
    ui();
  }
  EXPECT_TRUE(flow.Busy());
  EXPECT_EQ(flow.Begin("t1", "m2", "other.pdf", true), AttachmentSaveFlow::Start::Ignored);
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::Picture, true, true, flow.Busy()), AttachmentTap::Ignore);
  Pump();
  EXPECT_FALSE(flow.Busy());
  EXPECT_EQ(flow.Begin("t1", "m2", "other.pdf", true), AttachmentSaveFlow::Start::Started) << "free again";
  EXPECT_EQ(dialogs_, 2);
}

// The save keeps its own state: a tap-open of the same message going on (the chat screen's own set) does not stop it,
// and it neither reads nor clears that set (ChatController wiring, chat_picture_wiring_test).
TEST_F(SaveFlowTest, ASaveDependsOnNothingButItsOwnState) {
  AttachmentSaveFlow flow(Ports());
  EXPECT_EQ(DecideAttachmentTap(kDesktop, AttachmentExitKind::Picture, true, true, flow.Busy()), AttachmentTap::Open);
  ASSERT_EQ(flow.Begin("t1", "m1", "report.gz", true), AttachmentSaveFlow::Start::Started)
      << "an open of m1 in progress elsewhere is no part of the save";
  Answer({SaveChoice::Kind::Cancelled, {}});
  Pump();
  EXPECT_FALSE(flow.Busy());
}

} // namespace
} // namespace pbr
