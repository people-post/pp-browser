// "Send run logs" packaging (DEV/PLAN-send-run-logs.md): which files go in (the same retention that prunes them),
// the record-by-record pre-filter, the gzip under a size limit, and that nothing is written to disk.
#include "foundation/diagnostics/RunLogBundle.h"

#include <zlib.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace pbr {
namespace {

namespace fs = std::filesystem;
using Clock = fs::file_time_type::clock;

std::string Gunzip(const std::vector<uint8_t>& bytes) {
  z_stream stream{};
  EXPECT_EQ(inflateInit2(&stream, 15 + 16), Z_OK);
  stream.next_in = const_cast<Bytef*>(bytes.data());
  stream.avail_in = static_cast<uInt>(bytes.size());
  std::string out;
  char buffer[65536];
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef*>(buffer);
    stream.avail_out = sizeof(buffer);
    result = inflate(&stream, Z_NO_FLUSH);
    out.append(buffer, sizeof(buffer) - stream.avail_out);
  } while (result == Z_OK);
  inflateEnd(&stream);
  EXPECT_EQ(result, Z_STREAM_END);
  return out;
}

LogFileInfo Info(const std::string& name, const int hours_old, const std::uintmax_t size, const bool current = false) {
  LogFileInfo info;
  info.name = name;
  info.mtime = Clock::now() - std::chrono::hours(hours_old);
  info.size = size;
  info.current = current;
  return info;
}

std::vector<std::string> Names(const RunLogSelection& selection) {
  std::vector<std::string> names;
  for (const RunLogPick& pick : selection.files) {
    names.push_back(fs::path(pick.name).filename().string());
  }
  return names;
}

// --- choosing the files ------------------------------------------------------------------------------------------

TEST(RunLogSelectTest, EverythingThatFitsGoesOldestFirst) {
  const auto selection = RunLogBundle::Select(
      {Info("/l/pp-browser.log", 0, 10, true), Info("/l/pp-browser.1.log", 5, 10), Info("/l/pp-browser.2.log", 9, 10)},
      Clock::now(), LogFile::EffectiveRetention(true), 100);
  EXPECT_EQ(Names(selection), (std::vector<std::string>{"pp-browser.2.log", "pp-browser.1.log", "pp-browser.log"}));
  EXPECT_FALSE(selection.truncated);
  for (const RunLogPick& pick : selection.files) {
    EXPECT_EQ(pick.take_bytes, pick.size);
  }
}

TEST(RunLogSelectTest, WhatDoesNotFitLeavesOutTheOldestAndCutsTheFirstMissToItsTail) {
  const auto selection = RunLogBundle::Select(
      {Info("/l/pp-browser.log", 0, 40, true), Info("/l/pp-browser.1.log", 5, 40), Info("/l/pp-browser.2.log", 9, 40)},
      Clock::now(), LogFile::EffectiveRetention(true), 60);
  EXPECT_EQ(Names(selection), (std::vector<std::string>{"pp-browser.1.log", "pp-browser.log"}));
  EXPECT_TRUE(selection.truncated);
  EXPECT_EQ(selection.files[0].take_bytes, 20u) << "the rest of the budget, from its tail";
  EXPECT_EQ(selection.files[1].take_bytes, 40u);
}

TEST(RunLogSelectTest, OneFileOverTheLimitIsCutToItsTail) {
  const auto selection = RunLogBundle::Select({Info("/l/pp-browser.log", 0, 1000, true)}, Clock::now(),
                                              LogFile::EffectiveRetention(true), 300);
  ASSERT_EQ(selection.files.size(), 1u);
  EXPECT_EQ(selection.files[0].take_bytes, 300u);
  EXPECT_TRUE(selection.truncated);
}

// Off (or unknown): only this launch and the one before, per family, however many older files are on disk.
TEST(RunLogSelectTest, OffTakesTwoLaunchesPerFamilyWhateverIsOnDisk) {
  std::vector<LogFileInfo> files;
  for (const std::string family : {"pp-browser", "metrics", "diag"}) {
    files.push_back(Info("/l/" + family + ".log", 0, 10, true));
    for (int i = 1; i <= 6; ++i) {
      files.push_back(Info("/l/" + family + "." + std::to_string(i) + ".log", i * 3, 10));
    }
  }
  const auto selection = RunLogBundle::Select(files, Clock::now(), LogFile::EffectiveRetention(false), 1u << 30);
  std::vector<std::string> names = Names(selection);
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::string>{"diag.1.log", "diag.log", "metrics.1.log", "metrics.log",
                                             "pp-browser.1.log", "pp-browser.log"}));
}

TEST(RunLogSelectTest, OnTakesTheSevenDays) {
  const auto selection =
      RunLogBundle::Select({Info("/l/pp-browser.log", 0, 10, true), Info("/l/pp-browser.1.log", 30, 10),
                            Info("/l/pp-browser.2.log", 24 * 6, 10), Info("/l/pp-browser.3.log", 24 * 8, 10)},
                           Clock::now(), LogFile::EffectiveRetention(true), 1u << 30);
  EXPECT_EQ(Names(selection), (std::vector<std::string>{"pp-browser.2.log", "pp-browser.1.log", "pp-browser.log"}));
}

// --- can it be sent ----------------------------------------------------------------------------------------------

class RunLogDirTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           (std::string("pp-run-log-bundle-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_ / "logs");
    log_path_ = LogFile::DefaultPath(dir_.string());
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  void Write(const std::string& path, const std::string& text, const int hours_old = 0) {
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << text;
    }
    fs::last_write_time(path, Clock::now() - std::chrono::hours(hours_old) - std::chrono::minutes(1));
  }
  std::vector<std::string> Listing() const {
    std::vector<std::string> names;
    for (const auto& entry : fs::recursive_directory_iterator(dir_)) {
      names.push_back(entry.path().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  }
  fs::path dir_;
  std::string log_path_;
};

TEST_F(RunLogDirTest, OffWithOnlyThisLaunchCannotBeSentWhateverTheOtherFamiliesHold) {
  Write(log_path_, "[2026-11-19 10:00:00.000] [INFO] hi\n");
  Write(LogFile::MetricsPath(log_path_), "m\n");
  Write(LogFile::RotatedPath(LogFile::MetricsPath(log_path_), 1), "m\n", 5);
  Write(LogFile::DiagPath(log_path_), "d\n");
  Write(LogFile::RotatedPath(LogFile::DiagPath(log_path_), 1), "d\n", 5);
  EXPECT_FALSE(RunLogBundle::Sendable(LogFile::FamilyFiles(log_path_), /*collect=*/false));
  EXPECT_TRUE(RunLogBundle::Sendable(LogFile::FamilyFiles(log_path_), /*collect=*/true));
  Write(LogFile::RotatedPath(log_path_, 1), "[2026-11-18 10:00:00.000] [INFO] before\n", 20);
  EXPECT_TRUE(RunLogBundle::Sendable(LogFile::FamilyFiles(log_path_), /*collect=*/false));
}

// --- the pre-filter ----------------------------------------------------------------------------------------------

constexpr const char* kHead = "[2026-11-19 10:00:00.123] [INFO] [Chat] ";

std::string Rec(const std::string& body) {
  return std::string(kHead) + body + "\n";
}

TEST(RunLogFilterTest, LinesWithoutTheKeysStayAsTheyAre) {
  const std::string text = Rec("relay poll ok status=200 ms=41") + Rec("hostname=relay.example err=ETIMEDOUT") +
                           Rec("context=boot filepath=x device_name=ipad") + Rec("draw=3 username=u1");
  EXPECT_EQ(RunLogBundle::FilterRecords(text), text);
}

TEST(RunLogFilterTest, EachDropKeyTakesTheWholeRecord) {
  for (const std::string key : {"text", "body", "content", "query", "question", "prompt", "draft", "caption", "raw"}) {
    const std::string text = Rec("before") + Rec("send thread=t1 " + key + "=hello there") + Rec("after");
    EXPECT_EQ(RunLogBundle::FilterRecords(text), Rec("before") + Rec("after")) << key;
  }
}

TEST(RunLogFilterTest, AMultiLineRecordGoesWhole) {
  const std::string text = Rec("before") + std::string(kHead) + "draft text=first line\nsecond line of the words\n" +
                           "third line\n" + Rec("after");
  EXPECT_EQ(RunLogBundle::FilterRecords(text), Rec("before") + Rec("after"));
}

TEST(RunLogFilterTest, EachMaskKeyHidesItsValueUpToTheNextKey) {
  for (const std::string key : {"name", "display_name", "nickname", "title", "filename", "path", "subtitle", "user"}) {
    EXPECT_EQ(RunLogBundle::FilterRecords(Rec("saved " + key + "=Mary Ann Lee err=EIO id=c1")),
              Rec("saved " + key + "=<hidden> err=EIO id=c1"))
        << key;
  }
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec("renamed (title=Our trip)")), Rec("renamed (title=<hidden>"))
      << "to the end";
}

TEST(RunLogFilterTest, AQuotedValueIsHiddenToItsClosingQuote) {
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec(R"(failed: [Pin/3] user="Mary said a=b" detail="vault locked")")),
            Rec(R"(failed: [Pin/3] user=<hidden> detail="vault locked")"));
}

TEST(RunLogFilterTest, HomeAndAppContainerPathsBecomeATilde) {
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec("crash dump /Users/kenneth/Library/x.txt kept")),
            Rec("crash dump ~/Library/x.txt kept"));
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec("dir /home/ann/.local/pp")), Rec("dir ~/.local/pp"));
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec(R"(dir C:\Users\Ann Lee\AppData\pp)")), Rec(R"(dir ~\AppData\pp)"))
      << "a Windows user name may hold a space";
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec(R"(dir C:\Users\ann\AppData\pp)")), Rec(R"(dir ~\AppData\pp)"));
  EXPECT_EQ(
      RunLogBundle::FilterRecords(Rec("open /private/var/mobile/Containers/Data/Application/0A1B-22/Documents/pp ok")),
      Rec("open ~/Documents/pp ok"));
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec("open /var/mobile/Containers/Data/Application/0A1B-22/Library")),
            Rec("open ~/Library"));
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec("open /data/user/0/dev.pp.app/files/logs")), Rec("open ~/files/logs"));
  EXPECT_EQ(RunLogBundle::FilterRecords(Rec("open /data/data/dev.pp.app/files")), Rec("open ~/files"));
}

TEST(RunLogFilterTest, LinesBeforeTheFirstRecordAreDropped) {
  EXPECT_EQ(RunLogBundle::FilterRecords("the end of a cut record\n" + Rec("next")), Rec("next"));
}

// --- the archive -------------------------------------------------------------------------------------------------

TEST(RunLogGzipTest, RoundTrips) {
  const std::string text = "hello\nworld\n" + std::string(10000, 'x');
  auto gz = RunLogBundle::Gzip(text);
  ASSERT_TRUE(gz) << gz.error().message;
  EXPECT_LT(gz->size(), text.size());
  EXPECT_EQ(Gunzip(*gz), text);
}

TEST_F(RunLogDirTest, TheArchiveIsTheFilteredFilesOldestFirstUpToTheLastCompleteLine) {
  Write(LogFile::RotatedPath(log_path_, 1), Rec("previous launch") + Rec("title=Secret Group err=0"), 3);
  Write(log_path_, Rec("this launch") + Rec("sent text=my words") + "[2026-11-19 10:00:01.000] [INFO] half writ");
  Write(LogFile::DiagPath(log_path_), "[2026-11-19 09:59:00.000] [INFO] [Diag] event=relay.poll\n", 1);

  const auto before = Listing();
  auto archive = RunLogBundle::Build(log_path_, LogFile::EffectiveRetention(true), 4 * 1024 * 1024, 0);
  ASSERT_TRUE(archive) << archive.error().message;
  EXPECT_EQ(Listing(), before) << "nothing written next to the logs";

  EXPECT_EQ(archive->mime, "application/gzip");
  EXPECT_EQ(archive->filename.rfind("pp-run-logs-", 0), 0u);
  EXPECT_EQ(archive->filename.substr(archive->filename.size() - 7), ".log.gz");
  EXPECT_FALSE(archive->truncated);
  EXPECT_EQ(archive->first_time, "11-19 10:00");
  EXPECT_EQ(Gunzip(archive->bytes), "==== pp-browser.1.log ====\n" + Rec("previous launch") +
                                        Rec("title=<hidden> err=0") + "==== diag.log ====\n" +
                                        "[2026-11-19 09:59:00.000] [INFO] [Diag] event=relay.poll\n" +
                                        "==== pp-browser.log ====\n" + Rec("this launch"));
}

TEST_F(RunLogDirTest, OffPackagesOnlyTwoLaunchesWhateverIsOnDisk) {
  Write(log_path_, Rec("now"));
  for (int i = 1; i <= 5; ++i) {
    Write(LogFile::RotatedPath(log_path_, static_cast<std::size_t>(i)), Rec("launch " + std::to_string(i)), i * 5);
  }
  auto archive = RunLogBundle::Build(log_path_, LogFile::EffectiveRetention(false), 4 * 1024 * 1024, 0);
  ASSERT_TRUE(archive) << archive.error().message;
  EXPECT_EQ(Gunzip(archive->bytes),
            "==== pp-browser.1.log ====\n" + Rec("launch 1") + "==== pp-browser.log ====\n" + Rec("now"));
  EXPECT_EQ(LogFile::FamilyFiles(log_path_).size(), 6u) << "packaging deletes nothing";
}

TEST_F(RunLogDirTest, AnArchiveOverTheLimitKeepsTheNewestRecordsAndFits) {
  std::mt19937 random(7);
  std::string noise; // does not compress: the archive must be cut
  while (noise.size() < 300 * 1024) {
    std::string line = kHead;
    for (int i = 0; i < 64; ++i) {
      line += static_cast<char>('!' + random() % 90);
    }
    noise += line + "\n";
  }
  Write(LogFile::RotatedPath(log_path_, 1), noise, 2);
  Write(log_path_, noise + Rec("the newest line"));
  const std::uintmax_t limit = 100 * 1024;
  auto archive = RunLogBundle::Build(log_path_, LogFile::EffectiveRetention(true), limit, 0);
  ASSERT_TRUE(archive) << archive.error().message;
  EXPECT_LE(archive->bytes.size(), limit);
  EXPECT_TRUE(archive->truncated);
  const std::string text = Gunzip(archive->bytes);
  EXPECT_NE(text.find("the newest line"), std::string::npos);
  EXPECT_EQ(text.find("==== pp-browser.1.log ===="), std::string::npos) << "the older launch did not fit";
}

TEST_F(RunLogDirTest, NoLogFilesIsAnError) {
  EXPECT_FALSE(RunLogBundle::Build(log_path_, LogFile::EffectiveRetention(true), 1024 * 1024, 0));
}

} // namespace
} // namespace pbr
