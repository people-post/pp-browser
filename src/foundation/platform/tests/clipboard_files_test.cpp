// DEV/PLAN-desktop-drop-paste.md, D2: the text/uri-list parser (pure) and the seam that makes Peek's `has_files` real.
// The native readers (NSPasteboard, CF_HDROP, SDL on Linux) are not tested here: the system clipboard is never touched.
#include "foundation/platform/ClipboardAttachments.h"
#include "foundation/platform/ClipboardFiles.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

using Files = std::vector<std::string>;

TEST(ParseUriListFilesTest, PlainFileUris) {
  EXPECT_EQ(ParseUriListFiles("file:///home/me/a.txt\r\nfile:///tmp/b.png\r\n"),
            (Files{"/home/me/a.txt", "/tmp/b.png"}));
}

TEST(ParseUriListFilesTest, PercentEncodedSpacesAndChineseAreDecodedAsUtf8) {
  EXPECT_EQ(ParseUriListFiles("file:///home/me/my%20file.txt"), Files{"/home/me/my file.txt"});
  EXPECT_EQ(ParseUriListFiles("file:///home/me/%E6%B5%8B%E8%AF%95.pdf"),
            Files{"/home/me/\xE6\xB5\x8B\xE8\xAF\x95.pdf"});
  EXPECT_EQ(ParseUriListFiles("file:///a/%e6%b5%8b.txt"), Files{"/a/\xE6\xB5\x8B.txt"}) << "lower-case hex";
  EXPECT_EQ(ParseUriListFiles("file:///a/100%25.txt"), Files{"/a/100%.txt"});
}

TEST(ParseUriListFilesTest, LocalhostHostIsAcceptedAndOtherHostsAreNot) {
  EXPECT_EQ(ParseUriListFiles("file://localhost/tmp/a.txt"), Files{"/tmp/a.txt"});
  EXPECT_EQ(ParseUriListFiles("FILE://LocalHost/tmp/a.txt"), Files{"/tmp/a.txt"});
  EXPECT_TRUE(ParseUriListFiles("file://server/share/a.txt").empty());
  EXPECT_TRUE(ParseUriListFiles("file://localhost.evil.com/a.txt").empty());
}

TEST(ParseUriListFilesTest, OtherSchemesAndNonUrisAreDropped) {
  EXPECT_TRUE(ParseUriListFiles("http://example.com/a.png").empty());
  EXPECT_TRUE(ParseUriListFiles("https://example.com/a.png\nftp://h/f\nsmb://h/s\n").empty());
  EXPECT_TRUE(ParseUriListFiles("/just/a/path").empty());
  EXPECT_TRUE(ParseUriListFiles("file:relative.txt").empty());
  EXPECT_TRUE(ParseUriListFiles("file://").empty());
  EXPECT_TRUE(ParseUriListFiles("file://host-only").empty());
  EXPECT_EQ(ParseUriListFiles("http://x/y\nfile:///keep.txt\nmailto:a@b"), Files{"/keep.txt"});
}

TEST(ParseUriListFilesTest, LineEndingsBlankLinesAndComments) {
  EXPECT_EQ(ParseUriListFiles("# a comment\r\n\r\nfile:///a\r\n\n\nfile:///b\n# another\rfile:///c"),
            (Files{"/a", "/b", "/c"}));
  EXPECT_EQ(ParseUriListFiles("  file:///padded  \r\n"), Files{"/padded"});
  EXPECT_TRUE(ParseUriListFiles("").empty());
  EXPECT_TRUE(ParseUriListFiles("\r\n\r\n").empty());
}

TEST(ParseUriListFilesTest, MalformedPercentSequencesDropThatLineOnly) {
  EXPECT_TRUE(ParseUriListFiles("file:///a/%").empty());
  EXPECT_TRUE(ParseUriListFiles("file:///a/%4").empty());
  EXPECT_TRUE(ParseUriListFiles("file:///a/%zz.txt").empty());
  EXPECT_TRUE(ParseUriListFiles("file:///a/%0").empty());
  EXPECT_TRUE(ParseUriListFiles("file:///a/%00b").empty()) << "an embedded NUL is never a path";
  EXPECT_EQ(ParseUriListFiles("file:///bad%2\nfile:///good"), Files{"/good"});
}

TEST(ParseUriListFilesTest, OrderIsKept) {
  EXPECT_EQ(ParseUriListFiles("file:///z\nfile:///a\nfile:///m"), (Files{"/z", "/a", "/m"}));
}

// ---- The Peek seam ---------------------------------------------------------------------------------------------------

TEST(ClipboardFilesSeamTest, PeekTakesHasFilesFromTheSourceFunctionNotTheTypeList) {
  ClipboardSource source;
  source.mime_types = []() { return std::vector<std::string>{"text/plain"}; };
  source.text = []() { return std::string("name.txt"); };
  source.has_files = []() { return true; };
  source.files_enabled = []() { return true; };
  const ClipboardSummary summary = PeekClipboard(source);
  EXPECT_TRUE(summary.has_files);
  EXPECT_TRUE(summary.files_enabled);

  source.has_files = []() { return false; };
  source.mime_types = []() { return std::vector<std::string>{"text/uri-list"}; };
  EXPECT_FALSE(PeekClipboard(source).has_files);
}

TEST(ClipboardFilesSeamTest, ASourceWithoutTheFunctionsIsNotEnabledAndReadsNothing) {
  ClipboardSource source;
  source.mime_types = []() { return std::vector<std::string>{"text/uri-list"}; };
  const ClipboardSummary summary = PeekClipboard(source);
  EXPECT_TRUE(summary.has_files);
  EXPECT_FALSE(summary.files_enabled);
  EXPECT_TRUE(ReadClipboardFileList(source).empty());
}

TEST(ClipboardFilesSeamTest, TheTestSourceFeedsTheNoArgumentCalls) {
  ClipboardSource source;
  source.mime_types = []() { return std::vector<std::string>{}; };
  source.has_files = []() { return true; };
  source.files_enabled = []() { return true; };
  source.read_files = []() { return Files{"/x/1", "/x/2"}; };
  SetClipboardSourceForTest(source);
  EXPECT_TRUE(PeekClipboard().has_files);
  EXPECT_EQ(ReadClipboardFileList(), (Files{"/x/1", "/x/2"}));
  SetClipboardSourceForTest(std::nullopt);
}

} // namespace
} // namespace pbr
