#include "feature/presenters/ThreadSearch.h"

#include "lib/messaging/QuoteReply.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace pbr;

namespace {

ThreadMessage Text(const std::string& id, int64_t order, const std::string& text,
                   ChatContentType type = ChatContentType::Text) {
  ThreadMessage m;
  m.id = id;
  m.display_order = order;
  m.text = text;
  m.content_type = type;
  return m;
}

std::vector<std::string> Ids(const std::vector<ThreadSearchHit>& hits) {
  std::vector<std::string> ids;
  for (const ThreadSearchHit& hit : hits) {
    ids.push_back(hit.message_id);
  }
  return ids;
}

std::vector<std::string> Search(const std::vector<ThreadMessage>& messages, const std::string& query) {
  return Ids(FindHits(BuildSearchIndex(messages), query));
}

} // namespace

TEST(ThreadSearchTest, AsciiCaseIsIgnored) {
  const std::vector<ThreadMessage> messages = {Text("a", 1, "Meet at the Airport"), Text("b", 2, "nothing")};
  EXPECT_EQ(Search(messages, "airport"), (std::vector<std::string>{"a"}));
  EXPECT_EQ(Search(messages, "AIRPORT"), (std::vector<std::string>{"a"}));
}

TEST(ThreadSearchTest, ChineseMatchesAsASubstring) {
  const std::vector<ThreadMessage> messages = {Text("a", 1, "明天下午三点开会"), Text("b", 2, "好的")};
  EXPECT_EQ(Search(messages, "下午"), (std::vector<std::string>{"a"}));
  EXPECT_EQ(Search(messages, "晚上"), (std::vector<std::string>{}));
}

TEST(ThreadSearchTest, EmptyOrBlankQueryFindsNothing) {
  const std::vector<ThreadMessage> messages = {Text("a", 1, "hello world")};
  EXPECT_TRUE(Search(messages, "").empty());
  EXPECT_TRUE(Search(messages, "   \t ").empty());
}

TEST(ThreadSearchTest, OnlyNonEmptyTextMessagesAreIndexed) {
  const std::vector<ThreadMessage> messages = {
      Text("text", 1, "keyword"),
      Text("recalled", 2, "keyword", ChatContentType::Deleted),
      Text("file", 3, "keyword.pdf", ChatContentType::Attachment),
      Text("event", 4, "keyword joined", ChatContentType::System),
      Text("card", 5, "keyword", ChatContentType::ContactCard),
      Text("blank", 6, "   "),
  };
  const std::vector<ThreadSearchEntry> index = BuildSearchIndex(messages);
  ASSERT_EQ(index.size(), 1U);
  EXPECT_EQ(index[0].message_id, "text");
  EXPECT_EQ(Search(messages, "keyword"), (std::vector<std::string>{"text"}));
}

TEST(ThreadSearchTest, MentionMatchesItsVisibleText) {
  const std::vector<ThreadMessage> messages = {Text("a", 1, "@张三 明天见"), Text("b", 2, "张三的书")};
  EXPECT_EQ(Search(messages, "@张"), (std::vector<std::string>{"a"}));
  EXPECT_EQ(Search(messages, "张"), (std::vector<std::string>{"b", "a"}));
}

TEST(ThreadSearchTest, HitsAreNewestFirst) {
  const std::vector<ThreadMessage> messages = {Text("old", 1, "tea"), Text("new", 30, "tea time"),
                                               Text("mid", 7, "green tea")};
  EXPECT_EQ(Search(messages, "tea"), (std::vector<std::string>{"new", "mid", "old"}));
}

TEST(ThreadSearchTest, QuoteReplyMatchesOnlyTheReply) {
  const std::vector<ThreadMessage> messages = {Text("r", 2, ComposeQuoteReply("sure, see you", "dinner at eight"))};
  EXPECT_EQ(Search(messages, "see you"), (std::vector<std::string>{"r"}));
  EXPECT_TRUE(Search(messages, "dinner").empty());
}

TEST(ThreadSearchTest, AiBlocksDocumentMatchesItsPlainText) {
  const std::string blocks =
      R"({"blocks":[{"type":"paragraph","text":"The capital of France is Paris."}]})";
  const std::vector<ThreadMessage> messages = {Text("ai", 1, blocks)};
  EXPECT_EQ(Search(messages, "paris"), (std::vector<std::string>{"ai"}));
  EXPECT_TRUE(Search(messages, "paragraph").empty());
}

TEST(ThreadSearchTest, RealignKeepsTheSameMessage) {
  const std::vector<ThreadSearchHit> before = {{"c", 3}, {"b", 2}, {"a", 1}};
  const std::vector<ThreadSearchHit> after = {{"d", 4}, {"c", 3}, {"b", 2}, {"a", 1}};
  EXPECT_EQ(RealignCurrent(before, 1, after), 2U);
}

TEST(ThreadSearchTest, RealignGoesToNewestWhenTheMessageIsGone) {
  const std::vector<ThreadSearchHit> before = {{"c", 3}, {"b", 2}, {"a", 1}};
  const std::vector<ThreadSearchHit> after = {{"c", 3}, {"a", 1}};
  EXPECT_EQ(RealignCurrent(before, 1, after), 0U);
  EXPECT_EQ(RealignCurrent({}, 0, after), 0U);
}
