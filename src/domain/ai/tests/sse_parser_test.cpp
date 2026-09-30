#include "common/PlatformLimits.h"
#include "domain/ai/SseParser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::vector<pbr::SseEvent> ParseWhole(const std::string& stream) {
  pbr::SseParser parser;
  std::vector<pbr::SseEvent> out;
  EXPECT_TRUE(parser.Feed(stream, out));
  return out;
}

std::vector<pbr::SseEvent> ParseByteByByte(const std::string& stream) {
  pbr::SseParser parser;
  std::vector<pbr::SseEvent> out;
  for (const char c : stream) {
    EXPECT_TRUE(parser.Feed(std::string_view(&c, 1), out));
  }
  return out;
}

} // namespace

TEST(SseParserTest, SingleEventDefaultsToMessage) {
  const auto events = ParseWhole("data: hello\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "hello");
}

TEST(SseParserTest, NamedEvent) {
  const auto events = ParseWhole("event: delta\ndata: {\"a\":1}\n\nevent: done\ndata: [DONE]\n\n");
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].event, "delta");
  EXPECT_EQ(events[0].data, "{\"a\":1}");
  EXPECT_EQ(events[1].event, "done");
  EXPECT_EQ(events[1].data, "[DONE]");
}

TEST(SseParserTest, MultiLineDataIsJoinedWithNewline) {
  const auto events = ParseWhole("data: one\ndata: two\ndata:\ndata:  four\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].data, "one\ntwo\n\n four"); // only one leading space is stripped
}

TEST(SseParserTest, CommentsIdRetryAndUnknownFieldsAreIgnored) {
  const auto events = ParseWhole(": keep-alive\nid: 7\nretry: 1000\nfoo: bar\ndata: x\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "x");
}

TEST(SseParserTest, AllLineEndings) {
  for (const char* eol : {"\n", "\r\n", "\r"}) {
    const std::string e = eol;
    const auto events = ParseWhole("event: a" + e + "data: 1" + e + e + "data: 2" + e + e);
    ASSERT_EQ(events.size(), 2u) << "eol size " << e.size();
    EXPECT_EQ(events[0].event, "a");
    EXPECT_EQ(events[0].data, "1");
    EXPECT_EQ(events[1].data, "2");
  }
}

TEST(SseParserTest, CrLfSplitAcrossChunks) {
  pbr::SseParser parser;
  std::vector<pbr::SseEvent> out;
  ASSERT_TRUE(parser.Feed("data: a\r", out));
  ASSERT_TRUE(parser.Feed("\n\r", out)); // "\r\n" ends the line, lone "\r" is the blank line
  ASSERT_EQ(out.size(), 1u);
  ASSERT_TRUE(parser.Feed("\ndata: b\r\n\r\n", out)); // this "\n" completes the blank line's "\r\n"
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].data, "a");
  EXPECT_EQ(out[1].data, "b");
}

TEST(SseParserTest, ByteByByteMatchesWholeIncludingSplitUtf8) {
  const std::string stream = "\xEF\xBB\xBF: hi\r\n"
                             "event: delta\r\ndata: 你好，世界\r\n\r\n"
                             "data: 第一行\ndata: 第二行 🙂\n\n"
                             "id: 3\r\rdata: tail\r\r";
  const auto whole = ParseWhole(stream);
  const auto bytes = ParseByteByByte(stream);
  ASSERT_EQ(whole.size(), 3u);
  ASSERT_EQ(bytes.size(), whole.size());
  for (size_t i = 0; i < whole.size(); ++i) {
    EXPECT_EQ(bytes[i].event, whole[i].event);
    EXPECT_EQ(bytes[i].data, whole[i].data);
  }
  EXPECT_EQ(whole[0].data, "你好，世界");
  EXPECT_EQ(whole[1].data, "第一行\n第二行 🙂");
}

TEST(SseParserTest, BlankLineWithoutDataDispatchesNothing) {
  const auto events = ParseWhole("\n\nevent: x\n\ndata: y\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].event, "message"); // the data-less event's name was reset
  EXPECT_EQ(events[0].data, "y");
}

TEST(SseParserTest, LeadingBomIsDropped) {
  const auto events = ParseWhole("\xEF\xBB\xBF" "data: bom\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].data, "bom");
}

TEST(SseParserTest, IncompleteEventIsNotEmitted) {
  EXPECT_TRUE(ParseWhole("data: partial\n").empty());
}

TEST(SseParserTest, OversizeLineReturnsFalse) {
  pbr::SseParser parser;
  std::vector<pbr::SseEvent> out;
  const std::string big(pbr::kMaxLlmResponseBytes + 1, 'x');
  EXPECT_FALSE(parser.Feed(big, out));
}
