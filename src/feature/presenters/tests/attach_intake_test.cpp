// The one entry for files going into a chat's send preview (DEV/PLAN-desktop-drop-paste.md, A): why files are refused
// and in what order, what the worker's facts turn a batch into, and the intake that ties them to ports.
#include "feature/presenters/AttachIntake.h"

#include "common/PathUtf8.h"
#include "lib/messaging/AttachmentLimits.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <chrono>
#include <string>
#include <vector>

namespace pbr {
namespace {

// ---- RefusalFor ----------------------------------------------------------------------------------------------------

TEST(AttachRefusalTest, AnOpenChatWithNothingInTheWayRefusesNothing) {
  EXPECT_EQ(RefusalFor(AttachState{}), AttachRefusal::None);
}

TEST(AttachRefusalTest, EachConditionAloneGivesItsOwnRefusal) {
  const std::vector<std::pair<std::function<void(AttachState&)>, AttachRefusal>> cases = {
      {[](AttachState& s) { s.ready = false; }, AttachRefusal::NotReady},
      {[](AttachState& s) { s.chat_open = false; }, AttachRefusal::NoChat},
      {[](AttachState& s) { s.attach_unavailable = true; }, AttachRefusal::AttachUnavailable},
      {[](AttachState& s) { s.ai_composer = true; }, AttachRefusal::AiComposer},
      {[](AttachState& s) { s.editing_message = true; }, AttachRefusal::Editing},
      {[](AttachState& s) { s.input_disabled = true; }, AttachRefusal::InputDisabled},
      {[](AttachState& s) { s.send_held = true; }, AttachRefusal::SendHeld},
      {[](AttachState& s) { s.preview_open = true; }, AttachRefusal::PreviewOpen},
      {[](AttachState& s) { s.layer_above = true; }, AttachRefusal::LayerAbove},
      {[](AttachState& s) { s.paste_in_flight = true; }, AttachRefusal::PasteInFlight},
      {[](AttachState& s) { s.attachment_busy = true; }, AttachRefusal::Busy},
  };
  for (size_t i = 0; i < cases.size(); ++i) {
    AttachState state;
    cases[i].first(state);
    EXPECT_EQ(RefusalFor(state), cases[i].second) << "case " << i;
  }
}

TEST(AttachRefusalTest, WhenSeveralHoldTheFirstInTheFixedOrderWins) {
  AttachState state;
  state.ready = false;
  state.chat_open = false;
  state.attach_unavailable = true;
  state.ai_composer = true;
  state.editing_message = true;
  state.input_disabled = true;
  state.send_held = true;
  state.preview_open = true;
  state.layer_above = true;
  state.paste_in_flight = true;
  state.attachment_busy = true;
  const std::vector<std::pair<AttachRefusal, std::function<void(AttachState&)>>> peel = {
      {AttachRefusal::NotReady, [](AttachState& s) { s.ready = true; }},
      {AttachRefusal::NoChat, [](AttachState& s) { s.chat_open = true; }},
      {AttachRefusal::AttachUnavailable, [](AttachState& s) { s.attach_unavailable = false; }},
      {AttachRefusal::AiComposer, [](AttachState& s) { s.ai_composer = false; }},
      {AttachRefusal::Editing, [](AttachState& s) { s.editing_message = false; }},
      {AttachRefusal::InputDisabled, [](AttachState& s) { s.input_disabled = false; }},
      {AttachRefusal::SendHeld, [](AttachState& s) { s.send_held = false; }},
      {AttachRefusal::PreviewOpen, [](AttachState& s) { s.preview_open = false; }},
      {AttachRefusal::LayerAbove, [](AttachState& s) { s.layer_above = false; }},
      {AttachRefusal::PasteInFlight, [](AttachState& s) { s.paste_in_flight = false; }},
      {AttachRefusal::Busy, [](AttachState& s) { s.attachment_busy = false; }},
  };
  for (const auto& [expected, clear] : peel) {
    EXPECT_EQ(RefusalFor(state), expected);
    clear(state);
  }
  EXPECT_EQ(RefusalFor(state), AttachRefusal::None);
}

TEST(AttachRefusalTest, EveryRefusalThatSpeaksHasALocaleKeyAndTheQuietOnesHaveNone) {
  EXPECT_EQ(AttachRefusalKey(AttachRefusal::None), nullptr);
  EXPECT_EQ(AttachRefusalKey(AttachRefusal::NotReady), nullptr) << "locked: nothing to say";
  EXPECT_EQ(AttachRefusalKey(AttachRefusal::PasteInFlight), nullptr) << "a paste is already being written";
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::NoChat), "chat.attach.no_chat");
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::AttachUnavailable), "chat.attach.not_here");
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::AiComposer), "chat.attach.not_here");
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::Editing), "chat.attach.not_here");
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::InputDisabled), "chat.attach.not_here");
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::SendHeld), "chat.send.held");
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::PreviewOpen), "chat.attach.finish_current");
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::LayerAbove), "chat.attach.not_here");
  EXPECT_STREQ(AttachRefusalKey(AttachRefusal::Busy), "chat.attach.previous_still_sending");
}

// ---- PlanAttachCandidates ------------------------------------------------------------------------------------------

FileFacts Regular(const SniffedType type, const uint64_t size = 1000) {
  FileFacts facts;
  facts.status = FileStatus::Regular;
  facts.size = size;
  facts.sniffed = type;
  return facts;
}

FileFacts WithStatus(const FileStatus status) {
  FileFacts facts;
  facts.status = status;
  return facts;
}

TEST(AttachPlanTest, TheKindComesFromTheContentAndTheVideoNameNotFromTheExtension) {
  const std::vector<AttachCandidate> candidates = {{"a.png"}, {"b.jpg"}, {"c.gif"}, {"d.webp"},
                                                   {"e.mp4"}, {"f.pdf"}, {"g.heic"}};
  const std::vector<FileFacts> facts = {
      Regular(SniffedType::Png),   Regular(SniffedType::Jpeg),  Regular(SniffedType::Gif),  Regular(SniffedType::Webp),
      Regular(SniffedType::Video), Regular(SniffedType::Other), Regular(SniffedType::Other)};
  const AttachPlan plan = PlanAttachCandidates(candidates, facts, 9);
  ASSERT_EQ(plan.items.size(), 7U);
  const std::vector<PictureSendKind> want = {
      PictureSendKind::Picture, PictureSendKind::Picture, PictureSendKind::Picture, PictureSendKind::Picture,
      PictureSendKind::Video,   PictureSendKind::File,    PictureSendKind::File};
  for (size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(plan.items[i].kind, want[i]) << candidates[i].path;
    EXPECT_EQ(plan.items[i].path, candidates[i].path) << "the order is kept";
  }
  EXPECT_EQ(plan.folders, 0);
  EXPECT_EQ(plan.unreadable, 0);
  EXPECT_FALSE(plan.truncated);
}

TEST(AttachPlanTest, AFolderIsDroppedAndCounted) {
  const AttachPlan plan =
      PlanAttachCandidates({{"dir"}, {"a.zip"}}, {WithStatus(FileStatus::Directory), Regular(SniffedType::Other)}, 9);
  ASSERT_EQ(plan.items.size(), 1U);
  EXPECT_EQ(plan.items[0].path, "a.zip");
  EXPECT_EQ(plan.folders, 1);
  EXPECT_EQ(plan.unreadable, 0);
}

TEST(AttachPlanTest, AMissingFileAndAStatusErrorAreDroppedAndCountedAsUnreadable) {
  const AttachPlan plan = PlanAttachCandidates(
      {{"gone"}, {"odd"}, {"a.zip"}},
      {WithStatus(FileStatus::Missing), WithStatus(FileStatus::Error), Regular(SniffedType::Other)}, 9);
  ASSERT_EQ(plan.items.size(), 1U);
  EXPECT_EQ(plan.unreadable, 2);
  EXPECT_EQ(plan.folders, 0);
}

TEST(AttachPlanTest, MoreThanTheCapKeepsTheFirstOnesAndFlagsIt) {
  std::vector<AttachCandidate> candidates;
  std::vector<FileFacts> facts;
  for (int i = 0; i < 12; ++i) {
    candidates.push_back({"f" + std::to_string(i) + ".zip"});
    facts.push_back(Regular(SniffedType::Other));
  }
  const AttachPlan plan = PlanAttachCandidates(candidates, facts, 9);
  ASSERT_EQ(plan.items.size(), 9U);
  EXPECT_TRUE(plan.truncated);
  EXPECT_EQ(plan.items.front().path, "f0.zip");
  EXPECT_EQ(plan.items.back().path, "f8.zip");
}

TEST(AttachPlanTest, ExactlyTheCapIsNotTruncatedAndDroppedOnesDoNotCountAgainstIt) {
  std::vector<AttachCandidate> candidates;
  std::vector<FileFacts> facts;
  for (int i = 0; i < 9; ++i) {
    candidates.push_back({"f" + std::to_string(i)});
    facts.push_back(Regular(SniffedType::Other));
  }
  EXPECT_FALSE(PlanAttachCandidates(candidates, facts, 9).truncated);
  candidates.insert(candidates.begin(), {"dir"});
  facts.insert(facts.begin(), WithStatus(FileStatus::Directory));
  const AttachPlan plan = PlanAttachCandidates(candidates, facts, 9);
  EXPECT_EQ(plan.items.size(), 9U) << "the folder took no place";
  EXPECT_FALSE(plan.truncated);
}

TEST(AttachPlanTest, NothingLeftGivesAnEmptyPlan) {
  const AttachPlan plan =
      PlanAttachCandidates({{"d1"}, {"d2"}}, {WithStatus(FileStatus::Directory), WithStatus(FileStatus::Directory)}, 9);
  EXPECT_TRUE(plan.items.empty());
  EXPECT_EQ(plan.folders, 2);
  EXPECT_TRUE(PlanAttachCandidates({}, {}, 9).items.empty());
}

TEST(AttachPlanTest, FewerFactsThanCandidatesCountTheRestAsUnreadable) {
  const AttachPlan plan = PlanAttachCandidates({{"a"}, {"b"}}, {Regular(SniffedType::Other)}, 9);
  EXPECT_EQ(plan.items.size(), 1U);
  EXPECT_EQ(plan.unreadable, 1);
}

TEST(AttachPlanTest, ABigFileCarriesItsSizeAndEndsUpTooLargeInTheQueue) {
  const uint64_t big = kDefaultAttachmentSendMaxBytes + 1;
  const AttachPlan plan = PlanAttachCandidates({{"/x/big.zip"}}, {Regular(SniffedType::Other, big)}, 9);
  ASSERT_EQ(plan.items.size(), 1U);
  EXPECT_EQ(plan.items[0].bytes, big);
  PictureSendPorts ports;
  ports.release = [](const std::string&) {};
  ports.remove_file = [](const std::string&) {};
  ports.now_ms = []() { return int64_t{0}; };
  ports.schedule_tick = [](int64_t) {};
  ports.offline = []() { return false; };
  ports.done = [](const PictureSendReport&) {};
  ports.send_item = [](const PictureSendItem&, bool) {};
  ports.send_text = [](const std::string&, const std::string&) { return true; };
  ports.put_back_text = [](const std::string&, const std::string&) {};
  PictureSendQueue queue("chat", AttachmentLimits{}, ports);
  const auto& item = plan.items[0];
  ASSERT_TRUE(queue.Add(item.path, item.kind, item.bytes, item.pixels, {}, item.owned));
  ASSERT_EQ(queue.Items().size(), 1U);
  EXPECT_TRUE(queue.Items()[0].too_large);
  EXPECT_EQ(queue.Items()[0].kind, PictureSendKind::File);
}

TEST(AttachPlanTest, OwnedCandidatesThatDoNotMakeItAreListedForDeletion) {
  std::vector<AttachCandidate> candidates = {
      {"keep.png", true}, {"gone.png", true}, {"mine.zip", false}, {"dir", false}};
  std::vector<FileFacts> facts = {Regular(SniffedType::Png), WithStatus(FileStatus::Missing),
                                  WithStatus(FileStatus::Missing), WithStatus(FileStatus::Directory)};
  const AttachPlan plan = PlanAttachCandidates(candidates, facts, 9);
  EXPECT_EQ(plan.discarded_owned, std::vector<std::string>{"gone.png"});
  EXPECT_TRUE(plan.items[0].owned);
}

TEST(AttachPlanTest, TheOwnedOnesPastTheCapAreListedForDeletionToo) {
  std::vector<AttachCandidate> candidates;
  std::vector<FileFacts> facts;
  for (int i = 0; i < 10; ++i) {
    candidates.push_back({"p" + std::to_string(i), true});
    facts.push_back(Regular(SniffedType::Png));
  }
  const AttachPlan plan = PlanAttachCandidates(candidates, facts, 9);
  EXPECT_EQ(plan.discarded_owned, std::vector<std::string>{"p9"});
}

TEST(AttachPlanTest, OneNotePerBatchFoldersFirstThenUnreadableThenTheCap) {
  AttachPlan plan;
  EXPECT_EQ(AttachPlanNoteKey(plan), nullptr);
  plan.truncated = true;
  EXPECT_STREQ(AttachPlanNoteKey(plan), "chat.attach.max_items");
  plan.unreadable = 2;
  EXPECT_STREQ(AttachPlanNoteKey(plan), "chat.attach.some_unreadable");
  plan.folders = 1;
  EXPECT_STREQ(AttachPlanNoteKey(plan), "chat.attach.no_folders");
}

// ---- GatherFileFacts: the head of a real file ----------------------------------------------------------------------

class GatherFactsTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_attach_intake_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string Write(const std::string& name, const std::string& bytes) {
    const auto path = dir_ / name;
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path.string();
  }
  static std::string Padded(std::string head) {
    head.resize(64, '\0');
    return head;
  }
  std::filesystem::path dir_;
};

TEST_F(GatherFactsTest, RealPictureHeadersAreRecognisedWhateverTheNameSays) {
  const std::string png = Padded(std::string("\x89PNG\r\n\x1a\n", 8));
  const std::string jpeg = Padded(std::string("\xFF\xD8\xFF\xE0", 4));
  const std::string gif = Padded("GIF89a");
  const std::string webp = Padded(std::string("RIFF\0\0\0\0WEBP", 12));
  EXPECT_EQ(GatherFileFacts(Write("a.png", png)).sniffed, SniffedType::Png);
  EXPECT_EQ(GatherFileFacts(Write("b.dat", jpeg)).sniffed, SniffedType::Jpeg);
  EXPECT_EQ(GatherFileFacts(Write("c", gif)).sniffed, SniffedType::Gif);
  EXPECT_EQ(GatherFileFacts(Write("d.bin", webp)).sniffed, SniffedType::Webp);
  const FileFacts facts = GatherFileFacts(Write("e.png", png));
  EXPECT_EQ(facts.status, FileStatus::Regular);
  EXPECT_EQ(facts.size, png.size());
}

TEST_F(GatherFactsTest, PicturesTheSendPathDoesNotKnowAreFilesAndSoIsATextFileNamedPng) {
  for (const char* name : {"a.heic", "b.psd", "c.svg", "d.bmp", "e.tiff", "noext"}) {
    EXPECT_EQ(GatherFileFacts(Write(name, Padded("not a known picture head"))).sniffed, SniffedType::Other) << name;
  }
  EXPECT_EQ(GatherFileFacts(Write("fake.png", "hello, I am text")).sniffed, SniffedType::Other);
  EXPECT_EQ(GatherFileFacts(Write("empty.png", "")).sniffed, SniffedType::Other);
}

TEST_F(GatherFactsTest, AVideoIsKnownByItsName) {
  EXPECT_EQ(GatherFileFacts(Write("clip.mp4", Padded("whatever"))).sniffed, SniffedType::Video);
  EXPECT_EQ(GatherFileFacts(Write("clip.mov", Padded("whatever"))).sniffed, SniffedType::Video);
  EXPECT_EQ(GatherFileFacts(Write("song.mp3", Padded("whatever"))).sniffed, SniffedType::Other) << "audio is a file";
}

// A path arrives as UTF-8 bytes (the system dialog, a drop, the clipboard). On Windows a narrow path is read as the ANSI
// code page, so a Chinese file name or folder was "unreadable" there; macOS passes either way, Windows CI is the check.
TEST_F(GatherFactsTest, AFileWithAChineseNameInAChineseFolderAndASpaceIsFoundSizedAndSniffed) {
  const std::filesystem::path folder =
      dir_ / PathFromUtf8("\xE5\x9B\xBE\xE7\x89\x87 \xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9");
  std::filesystem::create_directories(folder);
  const std::filesystem::path file = folder / PathFromUtf8("\xE6\xB5\x8B\xE8\xAF\x95 \xE5\x9B\xBE\xE7\x89\x87.png");
  const std::string png = Padded(std::string("\x89PNG\r\n\x1a\n", 8));
  {
    std::ofstream out(file, std::ios::binary);
    out.write(png.data(), static_cast<std::streamsize>(png.size()));
  }
  const FileFacts facts = GatherFileFacts(PathToUtf8(file)); // the UTF-8 bytes, as the dialog hands them
  EXPECT_EQ(facts.status, FileStatus::Regular);
  EXPECT_EQ(facts.size, png.size());
  EXPECT_EQ(facts.sniffed, SniffedType::Png);
  EXPECT_EQ(GatherFileFacts(PathToUtf8(folder)).status, FileStatus::Directory);
  EXPECT_EQ(GatherFileFacts(PathToUtf8(folder / PathFromUtf8("\xE6\xB2\xA1\xE6\x9C\x89.png"))).status,
            FileStatus::Missing);
}

TEST_F(GatherFactsTest, FolderMissingAndNotAPlainFileAreTold) {
  std::filesystem::create_directories(dir_ / "sub");
  EXPECT_EQ(GatherFileFacts((dir_ / "sub").string()).status, FileStatus::Directory);
  EXPECT_EQ(GatherFileFacts((dir_ / "nope.png").string()).status, FileStatus::Missing);
}

// ---- AttachIntake --------------------------------------------------------------------------------------------------

struct World {
  AttachState state;
  std::string chat = "chat-a";
  uint64_t session = 1;
  std::vector<std::function<void()>> worker;
  std::vector<std::function<void()>> ui;
  std::map<std::string, FileFacts> facts;
  std::vector<std::pair<std::string, int>> said;
  std::vector<std::string> released;
  struct Opened {
    std::string thread;
    std::vector<AttachPlannedItem> items;
  };
  std::vector<Opened> opened;
  bool open_ok = true;

  AttachIntakePorts Ports() {
    AttachIntakePorts ports;
    ports.state = [this]() { return state; };
    ports.chat_id = [this]() { return chat; };
    ports.session = [this]() { return session; };
    ports.post_worker = [this](std::function<void()> work) { worker.push_back(std::move(work)); };
    ports.post_ui = [this](std::function<void()> work) { ui.push_back(std::move(work)); };
    ports.gather = [this](const std::string& path) { return facts.at(path); };
    ports.say = [this](const char* key, const int n) { said.emplace_back(key, n); };
    ports.release_owned = [this](const std::string& path) { released.push_back(path); };
    ports.open_page = [this](const std::string& thread, std::vector<AttachPlannedItem> items) {
      opened.push_back({thread, std::move(items)});
      return open_ok;
    };
    return ports;
  }
  void RunWorker() {
    auto work = std::move(worker);
    worker.clear();
    for (auto& w : work) {
      w();
    }
  }
  void RunUi() {
    auto work = std::move(ui);
    ui.clear();
    for (auto& w : work) {
      w();
    }
  }
  void Settle() {
    RunWorker();
    RunUi();
  }
};

TEST(AttachIntakeTest, FactsAreReadOnAWorkerAndThePageOpensOnTheUiThread) {
  World world;
  world.facts["/a/x.png"] = Regular(SniffedType::Png, 10);
  world.facts["/a/y.pdf"] = Regular(SniffedType::Other, 20);
  AttachIntake intake(world.Ports());
  intake.Open({{"/a/x.png"}, {"/a/y.pdf"}}, AttachSource::Picker);
  EXPECT_TRUE(world.opened.empty());
  EXPECT_EQ(world.worker.size(), 1U) << "the file system is asked off the UI thread";
  world.RunWorker();
  EXPECT_TRUE(world.opened.empty()) << "and the page waits for the UI thread";
  world.RunUi();
  ASSERT_EQ(world.opened.size(), 1U);
  EXPECT_EQ(world.opened[0].thread, "chat-a");
  ASSERT_EQ(world.opened[0].items.size(), 2U);
  EXPECT_EQ(world.opened[0].items[0].kind, PictureSendKind::Picture);
  EXPECT_EQ(world.opened[0].items[1].kind, PictureSendKind::File);
  EXPECT_TRUE(world.said.empty());
}

TEST(AttachIntakeTest, ARefusedBatchSaysWhyAndNeverTouchesTheFileSystem) {
  World world;
  world.state.attachment_busy = true;
  AttachIntake intake(world.Ports());
  intake.Open({{"/a/x.png"}}, AttachSource::Drop);
  EXPECT_TRUE(world.worker.empty());
  ASSERT_EQ(world.said.size(), 1U);
  EXPECT_EQ(world.said[0].first, "chat.attach.previous_still_sending");
  EXPECT_TRUE(world.opened.empty());
}

TEST(AttachIntakeTest, AQuietRefusalSaysNothing) {
  World world;
  world.state.ready = false;
  AttachIntake intake(world.Ports());
  intake.Open({{"/a/x.png"}}, AttachSource::Picker);
  EXPECT_TRUE(world.said.empty());
  EXPECT_TRUE(world.worker.empty());
}

TEST(AttachIntakeTest, AnEmptyBatchDoesNothing) {
  World world;
  AttachIntake intake(world.Ports());
  intake.Open({}, AttachSource::Picker);
  EXPECT_TRUE(world.worker.empty());
  EXPECT_TRUE(world.said.empty());
}

TEST(AttachIntakeTest, FactsArrivingAfterTheChatChangedOpenNoPage) {
  World world;
  world.facts["/a/x.png"] = Regular(SniffedType::Png);
  AttachIntake intake(world.Ports());
  intake.Open({{"/a/x.png"}}, AttachSource::Picker);
  world.RunWorker();
  world.chat = "chat-b"; // the person opened another chat while the worker read the file
  world.RunUi();
  EXPECT_TRUE(world.opened.empty());
  ASSERT_EQ(world.said.size(), 1U);
  EXPECT_EQ(world.said[0].first, "chat.attach.not_sent_left_chat");
}

TEST(AttachIntakeTest, FactsArrivingAfterALockOrNewSessionOpenNoPage) {
  World world;
  world.facts["/a/x.png"] = Regular(SniffedType::Png);
  AttachIntake intake(world.Ports());
  intake.Open({{"/a/x.png"}}, AttachSource::Picker);
  world.RunWorker();
  world.state.ready = false;
  world.RunUi();
  EXPECT_TRUE(world.opened.empty());

  World again;
  again.facts["/a/x.png"] = Regular(SniffedType::Png);
  AttachIntake second(again.Ports());
  second.Open({{"/a/x.png"}}, AttachSource::Picker);
  again.RunWorker();
  again.session = 2; // unlocked again meanwhile: another session
  again.RunUi();
  EXPECT_TRUE(again.opened.empty());
}

TEST(AttachIntakeTest, AnotherPageOrUploadThatCameInMeanwhileRefusesWithItsOwnPill) {
  World world;
  world.facts["/a/x.png"] = Regular(SniffedType::Png);
  AttachIntake intake(world.Ports());
  intake.Open({{"/a/x.png"}}, AttachSource::Picker);
  world.RunWorker();
  world.state.preview_open = true;
  world.RunUi();
  EXPECT_TRUE(world.opened.empty());
  ASSERT_EQ(world.said.size(), 1U);
  EXPECT_EQ(world.said[0].first, "chat.attach.finish_current");
}

TEST(AttachIntakeTest, NothingLeftAfterTheFactsSaysWhyAndOpensNoPage) {
  World world;
  world.facts["/a/dir"] = WithStatus(FileStatus::Directory);
  AttachIntake intake(world.Ports());
  intake.Open({{"/a/dir"}}, AttachSource::Drop);
  world.Settle();
  EXPECT_TRUE(world.opened.empty());
  ASSERT_EQ(world.said.size(), 1U);
  EXPECT_EQ(world.said[0].first, "chat.attach.no_folders");

  World lost;
  lost.facts["/a/gone"] = WithStatus(FileStatus::Missing);
  AttachIntake second(lost.Ports());
  second.Open({{"/a/gone"}}, AttachSource::Picker);
  lost.Settle();
  EXPECT_TRUE(lost.opened.empty());
  ASSERT_EQ(lost.said.size(), 1U);
  EXPECT_EQ(lost.said[0].first, "chat.attach.some_unreadable");
  EXPECT_EQ(lost.said[0].second, 1);
}

TEST(AttachIntakeTest, TheFirstNineGoOnWithAPillAboutIt) {
  World world;
  std::vector<AttachCandidate> candidates;
  for (int i = 0; i < 11; ++i) {
    const std::string path = "/a/f" + std::to_string(i) + ".zip";
    candidates.push_back({path});
    world.facts[path] = Regular(SniffedType::Other);
  }
  AttachIntake intake(world.Ports());
  intake.Open(std::move(candidates), AttachSource::Drop);
  world.Settle();
  ASSERT_EQ(world.opened.size(), 1U);
  EXPECT_EQ(world.opened[0].items.size(), 9U);
  ASSERT_EQ(world.said.size(), 1U);
  EXPECT_EQ(world.said[0].first, "chat.attach.max_items");
}

TEST(AttachIntakeTest, TheReleaseHookRunsForOwnedFilesThatAreRefusedOrDroppedAndNeverForTheUsersOwn) {
  // Refused up front.
  {
    World world;
    world.state.editing_message = true;
    AttachIntake intake(world.Ports());
    intake.Open({{"/p/pasted1.png", true}, {"/u/mine.png", false}}, AttachSource::Paste);
    EXPECT_EQ(world.released, std::vector<std::string>{"/p/pasted1.png"});
  }
  // Dropped by the facts.
  {
    World world;
    world.facts["/p/pasted2.png"] = WithStatus(FileStatus::Missing);
    world.facts["/u/mine.png"] = WithStatus(FileStatus::Missing);
    world.facts["/u/ok.png"] = Regular(SniffedType::Png);
    AttachIntake intake(world.Ports());
    intake.Open({{"/p/pasted2.png", true}, {"/u/mine.png", false}, {"/u/ok.png", false}}, AttachSource::Paste);
    world.Settle();
    EXPECT_EQ(world.released, std::vector<std::string>{"/p/pasted2.png"});
  }
  // The chat changed meanwhile: every owned one goes, the user's own stay.
  {
    World world;
    world.facts["/p/pasted3.png"] = Regular(SniffedType::Png);
    world.facts["/u/mine.png"] = Regular(SniffedType::Png);
    AttachIntake intake(world.Ports());
    intake.Open({{"/p/pasted3.png", true}, {"/u/mine.png", false}}, AttachSource::Paste);
    world.RunWorker();
    world.chat = "chat-z";
    world.RunUi();
    EXPECT_EQ(world.released, std::vector<std::string>{"/p/pasted3.png"});
  }
  // The page could not open: open_page owns the items from the call on (the queue releases them, as the real one does),
  // so the intake must not release them a second time (N1: a reused file name would be deleted under its new owner).
  {
    World world;
    world.open_ok = false;
    world.facts["/p/pasted4.png"] = Regular(SniffedType::Png);
    world.facts["/u/mine.png"] = Regular(SniffedType::Png);
    AttachIntakePorts ports = world.Ports();
    ports.open_page = [&world](const std::string&, std::vector<AttachPlannedItem> items) {
      for (const AttachPlannedItem& item : items) {
        if (item.owned) {
          world.released.push_back(item.path); // what the queue's Back() does
        }
      }
      return false;
    };
    AttachIntake intake(std::move(ports));
    intake.Open({{"/p/pasted4.png", true}, {"/u/mine.png", false}}, AttachSource::Paste);
    world.Settle();
    EXPECT_EQ(world.released, std::vector<std::string>{"/p/pasted4.png"}) << "exactly one release";
  }
  // Opened: nothing is released here (the queue owns them now).
  {
    World world;
    world.facts["/p/pasted5.png"] = Regular(SniffedType::Png);
    AttachIntake intake(world.Ports());
    intake.Open({{"/p/pasted5.png", true}}, AttachSource::Paste);
    world.Settle();
    EXPECT_TRUE(world.released.empty());
  }
}

TEST(AttachIntakeTest, WithoutAReleasePortNothingIsDeletedAndNothingCrashes) {
  World world;
  world.state.editing_message = true;
  AttachIntakePorts ports = world.Ports();
  ports.release_owned = nullptr;
  AttachIntake intake(std::move(ports));
  intake.Open({{"/p/pasted.png", true}}, AttachSource::Paste);
  SUCCEED();
}

TEST(AttachIntakeTest, AnIntakeThatIsGoneBeforeTheWorkerReportsLeavesNoDanglingCall) {
  World world;
  world.facts["/a/x.png"] = Regular(SniffedType::Png);
  {
    AttachIntake intake(world.Ports());
    intake.Open({{"/a/x.png"}}, AttachSource::Picker);
    world.RunWorker();
  }
  world.RunUi();
  EXPECT_TRUE(world.opened.empty());
}

TEST(AttachIntakeTest, AChatWithoutAPlusButtonRefusesWithTheNotHerePillAndReleasesOwnedFiles) {
  World world;
  world.state.attach_unavailable = true; // a dissolved group, or messaging not ready
  AttachIntake intake(world.Ports());
  intake.Open({{"/p/pasted.png", true}, {"/u/mine.png", false}}, AttachSource::Drop);
  ASSERT_EQ(world.said.size(), 1U);
  EXPECT_EQ(world.said[0].first, "chat.attach.not_here");
  EXPECT_TRUE(world.worker.empty()) << "the file system is never touched";
  EXPECT_EQ(world.released, std::vector<std::string>{"/p/pasted.png"});
}

// S1: the worker must not touch the intake: it may be gone (the controller destroyed) by the time the files are read.
TEST(AttachIntakeTest, AnIntakeGoneBeforeTheWorkerRunsIsNeverTouchedAndTheOwnedFileIsReleased) {
  namespace fs = std::filesystem;
  const fs::path dir =
      fs::temp_directory_path() /
      ("pp_intake_gone_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  const fs::path owned = dir / "pasted.png";
  const fs::path mine = dir / "mine.png";
  std::ofstream(owned) << "x";
  std::ofstream(mine) << "x";

  World world;
  world.facts[owned.string()] = Regular(SniffedType::Png);
  world.facts[mine.string()] = Regular(SniffedType::Png);
  AttachIntakePorts ports = world.Ports();
  ports.release_owned = [](const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
  };
  {
    auto intake = std::make_unique<AttachIntake>(std::move(ports));
    intake->Open({{owned.string(), true}, {mine.string(), false}}, AttachSource::Paste);
    ASSERT_EQ(world.worker.size(), 1U);
  } // the intake is destroyed with its worker task still queued
  world.Settle();
  EXPECT_TRUE(world.opened.empty());
  EXPECT_TRUE(world.said.empty());
  EXPECT_FALSE(fs::exists(owned)) << "the file this app made is deleted";
  EXPECT_TRUE(fs::exists(mine)) << "the user's own file stays";
  std::error_code ec;
  fs::remove_all(dir, ec);
}

} // namespace
} // namespace pbr
