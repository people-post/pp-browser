#include "gui/chat/ChatSearchRows.h"
#include "gui/chat/ChatTranscriptScroller.h"
#include "lib/ui/ChatWidgetTypes.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace pbr;

namespace {

MessageDisplayRow Row(const std::string& id, int64_t order) {
  MessageDisplayRow row;
  row.message_id = id.c_str();
  row.display_order = order;
  return row;
}

std::vector<std::string> Marked(const std::vector<MessageDisplayRow>& rows, bool current) {
  std::vector<std::string> ids;
  for (const MessageDisplayRow& row : rows) {
    if (current ? row.search_current : row.search_hit) {
      ids.push_back(row.message_id.c_str());
    }
  }
  return ids;
}

/** A scroller over local state, with a row builder that counts its calls. */
struct ScrollerFixture {
  ui::Context* context = nullptr;
  bool show_jump = false;
  ui::String jump_label;
  std::vector<MessageDisplayRow> messages;
  bool has_turns = false;
  bool messaging_ready = true;
  int builds = 0;
  ChatTranscriptScroller scroller{context, {show_jump, jump_label, messages, has_turns}, messaging_ready};

  ScrollerFixture() {
    scroller.SetRowBuilder([this](const std::string&, std::optional<int64_t>, std::optional<int64_t>) {
      ++builds;
      return std::vector<MessageDisplayRow>{Row("a", 1), Row("b", 2), Row("c", 3), Row("d", 4)};
    });
  }
};

} // namespace

TEST(ChatSearchRowsTest, MarksHitsAndTheCurrentOne) {
  std::vector<MessageDisplayRow> rows = {Row("a", 1), Row("b", 2), Row("c", 3)};
  MarkSearchRows(rows, {"a", "c"}, "c");
  EXPECT_EQ(Marked(rows, false), (std::vector<std::string>{"a", "c"}));
  EXPECT_EQ(Marked(rows, true), (std::vector<std::string>{"c"}));
  MarkSearchRows(rows, {}, "");
  EXPECT_TRUE(Marked(rows, false).empty());
  EXPECT_TRUE(Marked(rows, true).empty());
}

TEST(ChatSearchRowsTest, DecoratorRunsOnBuiltRowsAndAnEmptyOneLeavesNoMarks) {
  ScrollerFixture f;
  f.scroller.SetRowDecorator([](std::vector<MessageDisplayRow>& rows) { MarkSearchRows(rows, {"b", "d"}, "d"); });
  f.messages = f.scroller.Rows("t", std::nullopt, std::nullopt);
  EXPECT_EQ(Marked(f.messages, false), (std::vector<std::string>{"b", "d"}));
  EXPECT_EQ(Marked(f.messages, true), (std::vector<std::string>{"d"}));

  f.scroller.SetRowDecorator({});
  f.scroller.Redecorate();
  EXPECT_TRUE(Marked(f.messages, false).empty());
  EXPECT_TRUE(Marked(f.messages, true).empty());
  // Freshly built rows carry no marks either.
  EXPECT_TRUE(Marked(f.scroller.Rows("t", std::nullopt, std::nullopt), false).empty());
}

TEST(ChatSearchRowsTest, RedecorateInPlaceWithoutRefetchingRows) {
  ScrollerFixture f;
  std::unordered_set<std::string> hits = {"a", "c"};
  std::string current = "c";
  f.scroller.SetRowDecorator(
      [&hits, &current](std::vector<MessageDisplayRow>& rows) { MarkSearchRows(rows, hits, current); });
  f.messages = f.scroller.Rows("t", std::nullopt, std::nullopt);
  ASSERT_EQ(f.builds, 1);
  EXPECT_EQ(Marked(f.messages, true), (std::vector<std::string>{"c"}));

  // A step: the current hit moves, the rows stay.
  current = "a";
  f.scroller.Redecorate();
  EXPECT_EQ(Marked(f.messages, true), (std::vector<std::string>{"a"}));
  EXPECT_EQ(Marked(f.messages, false), (std::vector<std::string>{"a", "c"}));

  // Another query: other hits, the rows stay.
  hits = {"b"};
  current = "b";
  f.scroller.Redecorate();
  EXPECT_EQ(Marked(f.messages, false), (std::vector<std::string>{"b"}));
  EXPECT_EQ(Marked(f.messages, true), (std::vector<std::string>{"b"}));
  EXPECT_EQ(f.builds, 1);
}
