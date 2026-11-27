// DEV/PLAN-desktop-drop-paste.md, D1: pasting a picture. The decision table, the paste itself (one at a time, the file
// written off the UI thread, every outcome clears the in-flight flag first), and — on the real file system, in the
// test's own temp folder — that every exit of a pasted file deletes it while a user's own file is never touched.
#include "feature/presenters/AttachIntake.h"
#include "feature/presenters/PictureSendQueue.h"
#include "foundation/platform/PastedFiles.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

namespace fs = std::filesystem;

// ---- DecidePaste: the table, row by row ------------------------------------------------------------------------------

ClipboardSummary Summary(const bool files, const bool text, const std::string& image, const bool unusable) {
  ClipboardSummary summary;
  summary.has_files = files;
  summary.has_text = text;
  summary.image_type = image;
  summary.has_unusable_image = unusable;
  return summary;
}

TEST(DecidePasteTest, FilesAreNotTakenOverInD1) {
  for (const bool text : {false, true}) {
    for (const char* image : {"", "image/png"}) {
      const PasteAction action = DecidePaste(Summary(true, text, image, false), AttachRefusal::None);
      EXPECT_FALSE(action.take_over);
      EXPECT_FALSE(action.read_image);
    }
  }
}

ClipboardSummary FilesSummary(const bool text, const std::string& image, const bool enabled = true) {
  ClipboardSummary summary = Summary(true, text, image, false);
  summary.files_enabled = enabled;
  return summary;
}

TEST(DecidePasteTest, CopiedFilesAreTakenOverAndReadWhateverElseIsThere) {
  for (const bool text : {false, true}) {
    for (const char* image : {"", "image/png"}) {
      const PasteAction action = DecidePaste(FilesSummary(text, image), AttachRefusal::None);
      EXPECT_TRUE(action.take_over) << text << image;
      EXPECT_TRUE(action.read_files) << text << image;
      EXPECT_FALSE(action.read_image) << text << image;
      EXPECT_EQ(action.pill_key, nullptr);
    }
  }
}

TEST(DecidePasteTest, CopiedFilesWithTheKillSwitchOffAreLeftToTheTextField) {
  for (const bool text : {false, true}) {
    const PasteAction action = DecidePaste(FilesSummary(text, "image/png", false), AttachRefusal::None);
    EXPECT_FALSE(action.take_over);
    EXPECT_FALSE(action.read_files);
    EXPECT_FALSE(action.read_image);
  }
}

TEST(DecidePasteTest, RefusedCopiedFilesAreSwallowedWithTheRefusalsPillAndNotRead) {
  for (const AttachRefusal refusal :
       {AttachRefusal::NotReady, AttachRefusal::NoChat, AttachRefusal::AiComposer, AttachRefusal::Editing,
        AttachRefusal::InputDisabled, AttachRefusal::SendHeld, AttachRefusal::PreviewOpen, AttachRefusal::LayerAbove,
        AttachRefusal::PasteInFlight, AttachRefusal::Busy}) {
    const PasteAction action = DecidePaste(FilesSummary(true, "image/png"), refusal);
    EXPECT_TRUE(action.take_over) << static_cast<int>(refusal);
    EXPECT_FALSE(action.read_files) << static_cast<int>(refusal);
    EXPECT_EQ(action.pill_key, AttachRefusalKey(refusal));
  }
}

TEST(DecidePasteTest, TextIsNeverTakenEvenWithAPictureBesideIt) {
  const PasteAction with_picture = DecidePaste(Summary(false, true, "image/png", false), AttachRefusal::None);
  EXPECT_FALSE(with_picture.take_over);
  const PasteAction unusable = DecidePaste(Summary(false, true, "", true), AttachRefusal::None);
  EXPECT_FALSE(unusable.take_over);
  const PasteAction text_only = DecidePaste(Summary(false, true, "", false), AttachRefusal::None);
  EXPECT_FALSE(text_only.take_over);
}

TEST(DecidePasteTest, OnlyAReadablePictureIsTakenAndRead) {
  const PasteAction action = DecidePaste(Summary(false, false, "image/png", false), AttachRefusal::None);
  EXPECT_TRUE(action.take_over);
  EXPECT_TRUE(action.read_image);
  EXPECT_EQ(action.pill_key, nullptr);
}

TEST(DecidePasteTest, OnlyAnUnusablePictureIsSwallowedWithTheUnsupportedPill) {
  const PasteAction action = DecidePaste(Summary(false, false, "", true), AttachRefusal::None);
  EXPECT_TRUE(action.take_over);
  EXPECT_FALSE(action.read_image);
  ASSERT_NE(action.pill_key, nullptr);
  EXPECT_STREQ(action.pill_key, "chat.paste.unsupported");
}

// The one DEBUG line per paste key: type names and flags only, never what is on the clipboard.
TEST(DecidePasteTest, TheDebugLineNamesTheTypesAndTheDecisionAndNothingFromTheClipboard) {
  ClipboardSummary summary = Summary(false, false, "image/png", false);
  PasteAction action = DecidePaste(summary, AttachRefusal::None);
  EXPECT_EQ(DescribePasteDecision(summary, action), "paste: files=0 text=0 image=image/png unusable=0 -> take over");
  summary = Summary(false, true, "", false);
  action = DecidePaste(summary, AttachRefusal::None);
  EXPECT_EQ(DescribePasteDecision(summary, action),
            "paste: files=0 text=1 image=none unusable=0 -> leave to the text field");
  summary = Summary(false, false, "", true);
  action = DecidePaste(summary, AttachRefusal::None);
  EXPECT_EQ(DescribePasteDecision(summary, action), "paste: files=0 text=0 image=none unusable=1 -> take over");
  summary = Summary(false, false, "image/jpeg", false);
  EXPECT_EQ(DescribePasteDecision(summary, DecidePaste(summary, AttachRefusal::None)),
            "paste: files=0 text=0 image=image/jpeg unusable=0 -> take over");
}

TEST(DecidePasteTest, AJpegIsTakenOverAndRead) {
  const PasteAction action = DecidePaste(Summary(false, false, "image/jpeg", false), AttachRefusal::None);
  EXPECT_TRUE(action.take_over);
  EXPECT_TRUE(action.read_image);
}

TEST(DecidePasteTest, NothingUsefulIsNotTaken) {
  EXPECT_FALSE(DecidePaste(Summary(false, false, "", false), AttachRefusal::None).take_over);
}

TEST(DecidePasteTest, ATakenPasteThatIsRefusedIsSwallowedWithTheRefusalsPill) {
  for (const AttachRefusal refusal :
       {AttachRefusal::NotReady, AttachRefusal::NoChat, AttachRefusal::AiComposer, AttachRefusal::Editing,
        AttachRefusal::InputDisabled, AttachRefusal::SendHeld, AttachRefusal::PreviewOpen, AttachRefusal::LayerAbove,
        AttachRefusal::PasteInFlight, AttachRefusal::Busy}) {
    for (const ClipboardSummary& clipboard :
         {Summary(false, false, "image/png", false), Summary(false, false, "", true)}) {
      const PasteAction action = DecidePaste(clipboard, refusal);
      EXPECT_TRUE(action.take_over) << static_cast<int>(refusal);
      EXPECT_FALSE(action.read_image) << static_cast<int>(refusal);
      EXPECT_EQ(action.pill_key, AttachRefusalKey(refusal)) << static_cast<int>(refusal) << ": its own pill, or none";
    }
  }
}

TEST(DecidePasteTest, AWriteInProgressAndALockedAppSwallowQuietly) {
  EXPECT_EQ(DecidePaste(Summary(false, false, "image/png", false), AttachRefusal::PasteInFlight).pill_key, nullptr);
  EXPECT_EQ(DecidePaste(Summary(false, false, "image/png", false), AttachRefusal::NotReady).pill_key, nullptr);
}

TEST(DecidePasteTest, ARefusedClipboardWithText) {
  // The text paste goes on as ever, whatever the chat would say about files.
  EXPECT_FALSE(DecidePaste(Summary(false, true, "image/png", false), AttachRefusal::Busy).take_over);
}

// ---- The paste itself ----------------------------------------------------------------------------------------------

const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "pixels";

class PasteAttachTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> counter{0};
    root_ = fs::temp_directory_path() /
            ("pp_paste_attach_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(counter++));
    pasted_ = root_ / "cache" / "pasted";
    fs::create_directories(root_);
    clipboard_ = Summary(false, false, "image/png", false);
    image_.bytes = kPng;
    image_.extension = "png";
    image_result_ = image_;
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  PastePorts Ports() {
    PastePorts ports;
    ports.state = [this]() { return state_; };
    ports.peek = [this]() {
      ++peeks_;
      return clipboard_;
    };
    ports.read_files = [this]() {
      ++file_reads_;
      return files_;
    };
    ports.has_text = [this]() {
      ++text_reads_;
      return clipboard_has_text_;
    };
    ports.read_image = [this]() {
      ++reads_;
      return image_result_;
    };
    ports.chat_id = [this]() { return chat_; };
    ports.session = [this]() { return session_; };
    ports.post_worker = [this](std::function<void()> work) { worker_.push_back(std::move(work)); };
    ports.post_ui = [this](std::function<void()> work) { ui_.push_back(std::move(work)); };
    ports.write = [this](const std::string& bytes, const std::string& extension) {
      ++writes_;
      if (fail_write_) {
        return std::optional<std::string>();
      }
      return WritePastedFile(pasted_, bytes, extension);
    };
    ports.say = [this](const char* key, int) { pills_.push_back(key); };
    ports.release_owned = [this](const std::string& path) { ReleasePastedFile(path, pasted_); };
    ports.open_preview = [this](std::vector<AttachCandidate> candidates) {
      for (AttachCandidate& candidate : candidates) {
        opened_.push_back(std::move(candidate));
      }
    };
    return ports;
  }
  /** The worker runs, then the UI task it posted. */
  void RunWorkerThenUi() {
    while (!worker_.empty()) {
      auto work = std::move(worker_.front());
      worker_.pop_front();
      work();
    }
    while (!ui_.empty()) {
      auto work = std::move(ui_.front());
      ui_.pop_front();
      work();
    }
  }
  void RunWorkerOnly() {
    while (!worker_.empty()) {
      auto work = std::move(worker_.front());
      worker_.pop_front();
      work();
    }
  }
  size_t FilesInFolder() const {
    std::error_code ec;
    if (!fs::is_directory(pasted_, ec)) {
      return 0;
    }
    return static_cast<size_t>(std::distance(fs::directory_iterator(pasted_), fs::directory_iterator()));
  }

  fs::path root_;
  fs::path pasted_;
  AttachState state_;
  ClipboardSummary clipboard_;
  ClipboardImage image_;
  std::optional<ClipboardImage> image_result_;
  std::string chat_ = "chat-a";
  uint64_t session_ = 1;
  bool fail_write_ = false;
  int peeks_ = 0;
  int reads_ = 0;
  int file_reads_ = 0;
  int text_reads_ = 0;
  bool clipboard_has_text_ = false;
  std::vector<std::string> files_;
  int writes_ = 0;
  std::vector<std::string> pills_;
  std::vector<AttachCandidate> opened_;
  std::deque<std::function<void()>> worker_;
  std::deque<std::function<void()>> ui_;
};

TEST_F(PasteAttachTest, APictureIsReadOnceWrittenOnAWorkerAndHandedOnAsAnOwnedFile) {
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(reads_, 1);
  EXPECT_EQ(writes_, 0) << "the write is posted to a worker, not done on the UI thread";
  EXPECT_TRUE(paste.InFlight());
  EXPECT_TRUE(opened_.empty());

  RunWorkerOnly();
  EXPECT_EQ(writes_, 1);
  EXPECT_TRUE(paste.InFlight()) << "back on the UI thread only after the UI task";
  RunWorkerThenUi();

  EXPECT_FALSE(paste.InFlight());
  ASSERT_EQ(opened_.size(), 1u);
  EXPECT_TRUE(opened_[0].owned);
  EXPECT_EQ(opened_[0].display_name, "Pasted image.png");
  EXPECT_TRUE(fs::exists(opened_[0].path));
  EXPECT_EQ(fs::path(opened_[0].path).parent_path(), pasted_);
  EXPECT_TRUE(pills_.empty());
}

TEST_F(PasteAttachTest, TextOnTheClipboardIsLeftToTheTextFieldAndNothingIsRead) {
  clipboard_ = Summary(false, true, "image/png", false);
  PasteAttach paste(Ports());
  EXPECT_FALSE(paste.TryPaste());
  EXPECT_EQ(reads_, 0);
  EXPECT_TRUE(worker_.empty());
  EXPECT_FALSE(paste.InFlight());
}

TEST_F(PasteAttachTest, CopiedFilesWithTheKillSwitchOffAreLeftToTheTextField) {
  clipboard_ = FilesSummary(false, "", false);
  PasteAttach paste(Ports());
  EXPECT_FALSE(paste.TryPaste());
  EXPECT_EQ(reads_, 0);
  EXPECT_EQ(file_reads_, 0);
}

TEST_F(PasteAttachTest, TwoCopiedFilesGoToThePreviewAsTwoNotOwnedCandidatesAtOnce) {
  clipboard_ = FilesSummary(true, "image/png"); // Finder: the name as text and an icon as a picture
  files_ = {"/a/one.txt", "/b/two.png"};
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(file_reads_, 1);
  EXPECT_EQ(reads_, 0) << "the picture is not read";
  ASSERT_EQ(opened_.size(), 2u);
  EXPECT_EQ(opened_[0].path, "/a/one.txt");
  EXPECT_EQ(opened_[1].path, "/b/two.png");
  EXPECT_FALSE(opened_[0].owned);
  EXPECT_FALSE(opened_[1].owned);
  EXPECT_TRUE(opened_[0].display_name.empty());
  EXPECT_TRUE(worker_.empty()) << "nothing is written";
  EXPECT_FALSE(paste.InFlight());
  EXPECT_TRUE(pills_.empty());
}

TEST_F(PasteAttachTest, CopiedFilesThatReadBackEmptyAreSwallowedWithTheCouldNotPastePill) {
  clipboard_ = FilesSummary(false, ""); // and no text to paste instead
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(pills_, std::vector<std::string>{"chat.paste.failed"});
  EXPECT_TRUE(opened_.empty());
}

// S2: files that read back empty must not eat a text paste (Finder also puts the name there; a stale types list).
TEST_F(PasteAttachTest, CopiedFilesThatReadBackEmptyWithTextOnTheClipboardLeaveThePasteToTheTextField) {
  clipboard_ = FilesSummary(false, "");
  clipboard_has_text_ = true;
  PasteAttach paste(Ports());
  EXPECT_FALSE(paste.TryPaste());
  EXPECT_TRUE(pills_.empty()) << "no pill: the text goes in";
  EXPECT_TRUE(opened_.empty());
  EXPECT_FALSE(paste.InFlight());
  EXPECT_EQ(text_reads_, 1);
}

TEST_F(PasteAttachTest, TheTextIsOnlyLookedAtWhenTheFilesReadBackEmpty) {
  clipboard_ = FilesSummary(true, "");
  clipboard_has_text_ = true;
  files_ = {"/a/one.txt"};
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(text_reads_, 0) << "read lazily, not on every paste";
  ASSERT_EQ(opened_.size(), 1u);
}

TEST_F(PasteAttachTest, RefusedCopiedFilesAreSwallowedWithThePillAndNotRead) {
  clipboard_ = FilesSummary(false, "");
  files_ = {"/a/one.txt"};
  state_.attachment_busy = true;
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(pills_, std::vector<std::string>{"chat.attach.previous_still_sending"});
  EXPECT_EQ(file_reads_, 0);
  EXPECT_TRUE(opened_.empty());
}

// N4: while a picture is being written, plain text still pastes.
TEST_F(PasteAttachTest, APasteOfPlainTextWhileAPictureIsBeingWrittenGoesToTheTextField) {
  PasteAttach paste(Ports());
  ASSERT_TRUE(paste.TryPaste()); // a picture, in flight
  clipboard_ = Summary(false, true, "", false);
  EXPECT_FALSE(paste.TryPaste());
  EXPECT_TRUE(pills_.empty());
  EXPECT_EQ(reads_, 1);
}

TEST_F(PasteAttachTest, APasteWhileAPictureIsBeingWrittenSwallowsCopiedFilesToo) {
  PasteAttach paste(Ports());
  ASSERT_TRUE(paste.TryPaste()); // a picture, in flight
  clipboard_ = FilesSummary(false, "");
  files_ = {"/a/one.txt"};
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(file_reads_, 0);
  EXPECT_TRUE(opened_.empty());
}

TEST_F(PasteAttachTest, AnUnusablePictureIsSwallowedWithItsPillAndNothingIsRead) {
  clipboard_ = Summary(false, false, "", true);
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(pills_, std::vector<std::string>{"chat.paste.unsupported"});
  EXPECT_EQ(reads_, 0);
  EXPECT_FALSE(paste.InFlight());
}

TEST_F(PasteAttachTest, ARefusedPasteIsSwallowedWithTheRefusalsPillAndNothingIsRead) {
  state_.attachment_busy = true;
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(pills_, std::vector<std::string>{"chat.attach.previous_still_sending"});
  EXPECT_EQ(reads_, 0);
  EXPECT_TRUE(worker_.empty());
}

TEST_F(PasteAttachTest, APictureTooLargeSaysSoAndWritesNothing) {
  image_result_ = ClipboardImage{{}, {}, true};
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(pills_, std::vector<std::string>{"chat.paste.too_large"});
  EXPECT_FALSE(paste.InFlight());
  EXPECT_TRUE(worker_.empty());
}

TEST_F(PasteAttachTest, APictureThatCannotBeReadAnyMoreIsUnsupported) {
  image_result_.reset();
  PasteAttach paste(Ports());
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(pills_, std::vector<std::string>{"chat.paste.unsupported"});
  EXPECT_FALSE(paste.InFlight());
}

TEST_F(PasteAttachTest, ASecondPasteWhileOneIsBeingWrittenIsSwallowedWithNoPillAndNoSecondRead) {
  PasteAttach paste(Ports());
  ASSERT_TRUE(paste.TryPaste());
  EXPECT_TRUE(paste.TryPaste()) << "a picture again: ours, quietly";
  EXPECT_TRUE(paste.TryPaste());
  EXPECT_EQ(reads_, 1) << "the picture is not read twice";
  EXPECT_TRUE(pills_.empty());
  RunWorkerThenUi();
  EXPECT_EQ(writes_, 1);
  EXPECT_EQ(opened_.size(), 1u);
}

TEST_F(PasteAttachTest, AWriteFailureClearsTheFlagSaysSoLeavesNothingAndALaterPasteWorks) {
  fail_write_ = true;
  PasteAttach paste(Ports());
  ASSERT_TRUE(paste.TryPaste());
  RunWorkerThenUi();
  EXPECT_FALSE(paste.InFlight());
  EXPECT_EQ(pills_, std::vector<std::string>{"chat.paste.failed"});
  EXPECT_TRUE(opened_.empty());
  EXPECT_EQ(FilesInFolder(), 0u);

  fail_write_ = false;
  pills_.clear();
  EXPECT_TRUE(paste.TryPaste());
  RunWorkerThenUi();
  EXPECT_EQ(reads_, 2);
  EXPECT_EQ(opened_.size(), 1u);
  EXPECT_TRUE(pills_.empty());
}

TEST_F(PasteAttachTest, ARealWriteFailureOnDiskLeavesNoPartialFile) {
  fs::create_directories(pasted_.parent_path());
  std::ofstream(pasted_) << "a plain file where the folder should be";
  PastePorts ports = Ports();
  ports.write = [this](const std::string& bytes, const std::string& extension) {
    return WritePastedFile(pasted_, bytes, extension);
  };
  PasteAttach paste(std::move(ports));
  ASSERT_TRUE(paste.TryPaste());
  RunWorkerThenUi();
  EXPECT_FALSE(paste.InFlight());
  EXPECT_EQ(pills_, std::vector<std::string>{"chat.paste.failed"});
  EXPECT_TRUE(fs::is_regular_file(pasted_)) << "what was in the way is untouched";
}

TEST_F(PasteAttachTest, TheChatChangingMeanwhileDeletesTheFileAndOpensNoPage) {
  PasteAttach paste(Ports());
  ASSERT_TRUE(paste.TryPaste());
  RunWorkerOnly();
  ASSERT_EQ(FilesInFolder(), 1u);
  chat_ = "chat-b";
  RunWorkerThenUi();
  EXPECT_FALSE(paste.InFlight());
  EXPECT_TRUE(opened_.empty());
  EXPECT_EQ(FilesInFolder(), 0u);
  EXPECT_TRUE(pills_.empty());
}

TEST_F(PasteAttachTest, ALockMeanwhileDeletesTheFileAndOpensNoPage) {
  PasteAttach paste(Ports());
  ASSERT_TRUE(paste.TryPaste());
  RunWorkerOnly();
  ++session_;
  RunWorkerThenUi();
  EXPECT_FALSE(paste.InFlight());
  EXPECT_TRUE(opened_.empty());
  EXPECT_EQ(FilesInFolder(), 0u);
}

TEST_F(PasteAttachTest, TheOwnerGoneBeforeTheWriteReturnsStillDeletesTheFile) {
  {
    PasteAttach paste(Ports());
    ASSERT_TRUE(paste.TryPaste());
    RunWorkerOnly();
    ASSERT_EQ(FilesInFolder(), 1u);
  }
  RunWorkerThenUi(); // the UI task of a PasteAttach that no longer exists
  EXPECT_EQ(FilesInFolder(), 0u);
  EXPECT_TRUE(opened_.empty());
}

TEST_F(PasteAttachTest, ThePasteFlagIsPartOfWhatRefusesOtherFiles) {
  PasteAttach paste(Ports());
  ASSERT_TRUE(paste.TryPaste());
  AttachState state;
  state.paste_in_flight = paste.InFlight();
  EXPECT_EQ(RefusalFor(state), AttachRefusal::PasteInFlight);
}

// ---- Every exit of a pasted file leaves nothing on disk; a user's own file is never touched ------------------------------

/** The queue's world with the real owned-file release and the desktop's no-op release for the user's own files. */
class OwnedFileExitTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> counter{0};
    root_ = fs::temp_directory_path() /
            ("pp_owned_exit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(counter++));
    pasted_ = root_ / "cache" / "pasted";
    fs::create_directories(root_ / "mine");
  }
  void TearDown() override {
    queue_.reset();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  PictureSendPorts Ports() {
    PictureSendPorts ports;
    ports.send_item = [this](const PictureSendItem& item, bool) { in_flight_ = item.id; };
    ports.send_text = [](const std::string&, const std::string&) { return true; };
    ports.put_back_text = [](const std::string&, const std::string&) {};
    // The desktop's ReleasePickedFile deletes nothing of the user's; this stands in for it.
    ports.release = [this](const std::string& path) { released_plain_.push_back(path); };
    ports.release_owned = [this](const std::string& path) { ReleasePastedFile(path, pasted_); };
    ports.remove_file = [](const std::string&) {};
    ports.now_ms = []() { return int64_t{1000}; };
    ports.schedule_tick = [](int64_t) {};
    ports.offline = []() { return false; };
    ports.done = [](const PictureSendReport&) {};
    return ports;
  }
  void Build() { queue_ = std::make_unique<PictureSendQueue>("chat-a", DefaultAttachmentLimits(), Ports()); }
  std::string Pasted(const std::string& content = "png") {
    const auto path = WritePastedFile(pasted_, content, "png");
    EXPECT_TRUE(path.has_value());
    return *path;
  }
  std::string Mine(const std::string& name) {
    const fs::path path = root_ / "mine" / name;
    std::ofstream(path) << "my own file";
    return path.string();
  }
  /** One pasted picture and one of the user's own, queued. */
  void AddBoth() {
    pasted_path_ = Pasted();
    mine_path_ = Mine("report.pdf");
    ASSERT_TRUE(queue_->Add(pasted_path_, PictureSendKind::Picture, 10, 100, "Pasted image.png", true).has_value());
    ASSERT_TRUE(queue_->Add(mine_path_, PictureSendKind::File, 10).has_value());
  }
  void ExpectPastedGoneMineKept() {
    EXPECT_FALSE(fs::exists(pasted_path_)) << "the pasted file must be gone";
    EXPECT_TRUE(fs::exists(mine_path_)) << "the user's own file must stay";
  }

  fs::path root_;
  fs::path pasted_;
  std::unique_ptr<PictureSendQueue> queue_;
  uint64_t in_flight_ = 0;
  std::vector<std::string> released_plain_;
  std::string pasted_path_;
  std::string mine_path_;
};

TEST_F(OwnedFileExitTest, BackReleasesThePastedFileAndNotTheUsersOwn) {
  Build();
  AddBoth();
  queue_->Back();
  ExpectPastedGoneMineKept();
}

TEST_F(OwnedFileExitTest, RemovingTheItemReleasesIt) {
  Build();
  AddBoth();
  ASSERT_TRUE(queue_->Remove(queue_->Items()[0].id));
  ExpectPastedGoneMineKept();
}

TEST_F(OwnedFileExitTest, SendFinishedReleasesItAfterTheItemEndsSuccessOrFailure) {
  bool first = true;
  for (const AttachmentSendOutcome outcome : {AttachmentSendOutcome::Sent, AttachmentSendOutcome::FailedNotStaged}) {
    if (!first) {
      SetUp(); // a fresh folder for the second outcome (the fixture made the first)
    }
    first = false;
    Build();
    AddBoth();
    ASSERT_TRUE(queue_->Start());
    ASSERT_NE(in_flight_, 0u);
    EXPECT_TRUE(fs::exists(pasted_path_)) << "kept while its send runs: the source is read from it";
    const uint64_t id = in_flight_;
    in_flight_ = 0;
    queue_->OnItemFinished(id, outcome);
    EXPECT_FALSE(fs::exists(pasted_path_)) << "released the moment its send has ended";
    EXPECT_TRUE(fs::exists(mine_path_));
    TearDown();
  }
}

TEST_F(OwnedFileExitTest, ACopiedFileOfTheUserSurvivesBackAndEverySendOutcome) {
  // What a pasted copy (D2) queues: the user's own file, not owned. Whatever ends it, the source file is still there.
  for (const AttachmentSendOutcome outcome : {AttachmentSendOutcome::Sent, AttachmentSendOutcome::FailedNotStaged}) {
    Build();
    const std::string copied = Mine("copied.pdf");
    ASSERT_TRUE(queue_->Add(copied, PictureSendKind::File, 10, 0, "", false).has_value());
    ASSERT_TRUE(queue_->Start());
    ASSERT_NE(in_flight_, 0u);
    const uint64_t id = in_flight_;
    in_flight_ = 0;
    queue_->OnItemFinished(id, outcome);
    EXPECT_TRUE(fs::exists(copied)) << "after the send ended";
    queue_.reset();
  }
  Build();
  const std::string copied = Mine("copied-back.pdf");
  ASSERT_TRUE(queue_->Add(copied, PictureSendKind::File, 10, 0, "", false).has_value());
  queue_->Back();
  EXPECT_TRUE(fs::exists(copied)) << "after Back";
}

TEST_F(OwnedFileExitTest, ATooLargeItemIsReleasedWhenSendingStarts) {
  AttachmentLimits limits = DefaultAttachmentLimits();
  limits.max_file_bytes = 5;
  queue_ = std::make_unique<PictureSendQueue>("chat-a", limits, Ports());
  pasted_path_ = Pasted();
  mine_path_ = Mine("a.pdf");
  ASSERT_TRUE(queue_->Add(pasted_path_, PictureSendKind::File, 100, 0, "Pasted image.png", true).has_value());
  ASSERT_TRUE(queue_->Add(mine_path_, PictureSendKind::File, 1).has_value());
  queue_->Start();
  EXPECT_FALSE(fs::exists(pasted_path_));
  EXPECT_TRUE(fs::exists(mine_path_));
}

TEST_F(OwnedFileExitTest, ChatDeletedLockedAndSignedOutReleaseTheRest) {
  using Exit = void (PictureSendQueue::*)();
  bool first = true;
  for (const Exit exit : {&PictureSendQueue::ChatDeleted, &PictureSendQueue::Locked, &PictureSendQueue::SignedOut}) {
    if (!first) {
      SetUp();
    }
    first = false;
    Build();
    AddBoth();
    (queue_.get()->*exit)();
    ExpectPastedGoneMineKept();
    TearDown();
  }
}

TEST_F(OwnedFileExitTest, ALockWhileTheItemIsBeingSentReleasesItWhenItsEndComes) {
  Build();
  AddBoth();
  ASSERT_TRUE(queue_->Start());
  const uint64_t id = in_flight_;
  queue_->Locked();
  EXPECT_TRUE(fs::exists(pasted_path_)) << "still being read by its send";
  queue_->OnItemFinished(id, AttachmentSendOutcome::FailedNotStaged);
  ExpectPastedGoneMineKept();
}

TEST_F(OwnedFileExitTest, AFullQueueReleasesTheOwnedFileItRefuses) {
  Build();
  for (size_t i = 0; i < PictureSendQueue::kMaxItems; ++i) {
    ASSERT_TRUE(queue_->Add(Mine("f" + std::to_string(i) + ".pdf"), PictureSendKind::File, 1).has_value());
  }
  pasted_path_ = Pasted();
  mine_path_ = Mine("late.pdf");
  EXPECT_FALSE(queue_->Add(pasted_path_, PictureSendKind::Picture, 1, 1, "Pasted image.png", true).has_value());
  EXPECT_FALSE(queue_->Add(mine_path_, PictureSendKind::File, 1).has_value());
  ExpectPastedGoneMineKept();
}

TEST_F(OwnedFileExitTest, ADestroyedQueueThatNeverEndedDeletesTheOwnedFileOnly) {
  Build();
  AddBoth();
  queue_.reset();
  ExpectPastedGoneMineKept();
}

TEST_F(OwnedFileExitTest, AnItemWithoutAnOwnedPortStillGoesThroughRelease) {
  PictureSendPorts ports = Ports();
  ports.release_owned = nullptr;
  queue_ = std::make_unique<PictureSendQueue>("chat-a", DefaultAttachmentLimits(), std::move(ports));
  AddBoth();
  queue_->Back();
  EXPECT_EQ(released_plain_.size(), 2u) << "no owned port: the old path, nothing deleted by us";
  EXPECT_TRUE(fs::exists(pasted_path_));
}

} // namespace
} // namespace pbr
