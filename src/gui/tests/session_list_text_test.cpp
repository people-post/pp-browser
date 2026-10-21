#include "gui/chat/SessionListText.h"

#include <gtest/gtest.h>

#include <vector>

using namespace pbr;

namespace {

std::tm Day(const int year, const int month, const int day, const int yday, const int hour = 0, const int min = 0) {
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_yday = yday;
  tm.tm_hour = hour;
  tm.tm_min = min;
  return tm;
}

} // namespace

TEST(SessionListTextTest, ShortMessageBecomesTheTitleAsIs) {
  EXPECT_EQ(AiThreadTitleFromMessage("今天有什么新闻"), "今天有什么新闻");
  EXPECT_EQ(AiThreadTitleFromMessage("  hello \n\n world\t"), "hello world");
  EXPECT_EQ(AiThreadTitleFromMessage(" \n "), "");
}

TEST(SessionListTextTest, LongMessageIsCutOnACharacterBoundary) {
  const std::string title = AiThreadTitleFromMessage("请给我一条今天最新的政治新闻并且详细说明它的背景和各方的反应以及后续影响");
  EXPECT_EQ(title, "请给我一条今天最新的政治新闻并且详细说明它的背景…");

  const std::string latin = AiThreadTitleFromMessage("abcdefghijklmnopqrstuvwxyz0123456789");
  EXPECT_EQ(latin, "abcdefghijklmnopqrstuvwx…");

  // Exactly at the limit: no ellipsis.
  EXPECT_EQ(AiThreadTitleFromMessage("abcdefghijklmnopqrstuvwx"), "abcdefghijklmnopqrstuvwx");
}

TEST(SessionListTextTest, DateLabelShowsDateAndTimeThisYearAndFullDateBefore) {
  const std::tm now = Day(2026, 10, 3, 275, 18, 30);
  EXPECT_EQ(SessionDateLabel(Day(2026, 10, 3, 275, 9, 5), now), "10/3 09:05");
  EXPECT_EQ(SessionDateLabel(Day(2026, 10, 2, 274, 23, 59), now), "10/2 23:59");
  EXPECT_EQ(SessionDateLabel(Day(2026, 1, 15, 14), now), "1/15 00:00");
  EXPECT_EQ(SessionDateLabel(Day(2025, 12, 31, 364), now), "2025/12/31");
}

TEST(SessionListTextTest, UnsetTimestampHasNoLabel) {
  EXPECT_EQ(SessionDateLabel(int64_t{0}, int64_t{1700000000000}), "");
}

// A Home chip continues its own thread: the newest AI thread titled like the chip's sentence.
TEST(SessionListTextTest, ChipFindsItsNewestAiThread) {
  struct T {
    std::string id;
    bool ai;
    std::string title;
    int64_t updated;
  };
  const std::vector<T> threads = {
      {"a", true, "今天有什么新闻？", 10},
      {"b", true, "今天有什么新闻？", 30},
      {"c", false, "今天有什么新闻？", 99}, // a chat with a person named like that is not it
      {"d", true, "别的话题", 50},
  };
  const auto find = [&](std::string_view message) {
    return pbr::FindChipThreadId(
        threads, message, [](const T& t) { return t.ai; }, [](const T& t) -> const std::string& { return t.title; },
        [](const T& t) { return t.updated; }, [](const T& t) -> const std::string& { return t.id; });
  };
  EXPECT_EQ(find("今天有什么新闻？"), "b");
  EXPECT_EQ(find("  今天有什么新闻？\n"), "b"); // the title rule collapses whitespace
  EXPECT_EQ(find("给我看看最新的简报"), "");
  EXPECT_EQ(find(""), "");
}

TEST(SessionListTextTest, PreviewIsOneLineOfReadableText) {
  EXPECT_EQ(SessionPreviewLine("好的，\n明天  见\t！"), "好的， 明天 见 ！");
  EXPECT_EQ(SessionPreviewLine("  \n "), "");
}

TEST(SessionListTextTest, PreviewShowsLinkTextNotMarkdown) {
  // An AI answer's last message is Markdown; the row shows what the reader would see.
  EXPECT_EQ(SessionPreviewLine("1. [五角大楼停用 Anthropic 工具](https://www.bbc.com/zhongwen/a-1)\n2. [诺贝尔物理学奖](https://dw.com/x)"),
            "1. 五角大楼停用 Anthropic 工具 2. 诺贝尔物理学奖");
  EXPECT_EQ(SessionPreviewLine("**结论**：见 [原文](https://a.example/x) 和 [1]"), "结论：见 原文 和 [1]");
}

TEST(SessionListTextTest, PreviewIsCutOnACharacterBoundary) {
  const std::string cut = SessionPreviewLine("一二三四五六七八九十", 4);
  EXPECT_EQ(cut, "一二三四…");
}

TEST(SessionListTextTest, PreviewShowsTheTextOfStoredMarkup) {
  // An AI answer's preview is stored as rendered markup; the row shows its text, never the tags.
  EXPECT_EQ(SessionPreviewLine("<ol><li><span class=\"md-marker\">1.</span><div class=\"md-item\">五角大楼停用工具</div></li>"
                               "<li><span class=\"md-marker\">2.</span><div class=\"md-item\">诺贝尔物理学奖</div></li></ol>"),
            "1. 五角大楼停用工具 2. 诺贝尔物理学奖");
  EXPECT_EQ(SessionPreviewLine("<div class=\"stack\"><p>Brief 最新简报（10 条）：</p><button class=\"x\">打开</button></div>"),
            "Brief 最新简报（10 条）： 打开");
  EXPECT_EQ(SessionPreviewLine("<p>A &amp; B &lt;tag&gt; &quot;q&quot; it&#39;s</p>"), "A & B <tag> \"q\" it's");
}

TEST(SessionListTextTest, PreviewKeepsAngleBracketsThatAreNotTags) {
  // A person's message is plain text: comparisons and emoticons stay as typed.
  EXPECT_EQ(SessionPreviewLine("1 < 2 而且 3 > 2"), "1 < 2 而且 3 > 2");
  EXPECT_EQ(SessionPreviewLine("爱你 <3"), "爱你 <3");
  EXPECT_EQ(SessionPreviewLine("a<b 没有右括号"), "a<b 没有右括号");
}
