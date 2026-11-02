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
  row.content_rml = ("<p>text " + id + "</p>").c_str();
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
  MarkSearchRows(rows, {"a", "c"}, "c", "x");
  EXPECT_EQ(Marked(rows, false), (std::vector<std::string>{"a", "c"}));
  EXPECT_EQ(Marked(rows, true), (std::vector<std::string>{"c"}));
  MarkSearchRows(rows, {}, "", "x");
  EXPECT_TRUE(Marked(rows, false).empty());
  EXPECT_TRUE(Marked(rows, true).empty());
}

TEST(ChatSearchRowsTest, DecoratorRunsOnBuiltRowsAndAnEmptyOneLeavesNoMarks) {
  ScrollerFixture f;
  f.scroller.SetRowDecorator([](std::vector<MessageDisplayRow>& rows) { MarkSearchRows(rows, {"b", "d"}, "d", "x"); });
  f.messages = f.scroller.Rows("t", std::nullopt, std::nullopt);
  EXPECT_EQ(Marked(f.messages, false), (std::vector<std::string>{"b", "d"}));
  EXPECT_EQ(Marked(f.messages, true), (std::vector<std::string>{"d"}));

  f.scroller.SetRowDecorator({});
  f.scroller.Redecorate();
  EXPECT_TRUE(Marked(f.messages, false).empty());
  for (const MessageDisplayRow& row : f.messages) {
    EXPECT_EQ(std::string(row.content_rml.c_str()).find("search-mark"), std::string::npos);
  }
  EXPECT_TRUE(Marked(f.messages, true).empty());
  // Freshly built rows carry no marks either.
  EXPECT_TRUE(Marked(f.scroller.Rows("t", std::nullopt, std::nullopt), false).empty());
}

TEST(ChatSearchRowsTest, RedecorateInPlaceWithoutRefetchingRows) {
  ScrollerFixture f;
  std::unordered_set<std::string> hits = {"a", "c"};
  std::string current = "c";
  f.scroller.SetRowDecorator(
      [&hits, &current](std::vector<MessageDisplayRow>& rows) { MarkSearchRows(rows, hits, current, "text"); });
  f.messages = f.scroller.Rows("t", std::nullopt, std::nullopt);
  ASSERT_EQ(f.builds, 1);
  EXPECT_EQ(Marked(f.messages, true), (std::vector<std::string>{"c"}));

  // A step: the current hit moves, the rows stay.
  current = "a";
  f.scroller.Redecorate();
  EXPECT_EQ(Marked(f.messages, true), (std::vector<std::string>{"a"}));
  // The marks moved with it, inside the text, on the rows already shown.
  EXPECT_NE(std::string(f.messages[0].content_rml.c_str()).find("search-mark-current"), std::string::npos);
  EXPECT_EQ(std::string(f.messages[2].content_rml.c_str()).find("search-mark-current"), std::string::npos);
  EXPECT_NE(std::string(f.messages[2].content_rml.c_str()).find("search-mark"), std::string::npos);
  EXPECT_EQ(Marked(f.messages, false), (std::vector<std::string>{"a", "c"}));

  // Another query: other hits, the rows stay.
  hits = {"b"};
  current = "b";
  f.scroller.Redecorate();
  EXPECT_EQ(Marked(f.messages, false), (std::vector<std::string>{"b"}));
  EXPECT_EQ(Marked(f.messages, true), (std::vector<std::string>{"b"}));
  EXPECT_EQ(f.builds, 1);
}

namespace {

/** Every tag closed in order (void tags and "/>" aside): what the RML parser needs to keep the row's markup. */
bool Balanced(const std::string& rml) {
  std::vector<std::string> open;
  for (size_t i = rml.find('<'); i != std::string::npos; i = rml.find('<', i + 1)) {
    const size_t end = rml.find('>', i);
    if (end == std::string::npos) {
      return false;
    }
    std::string tag = rml.substr(i + 1, end - i - 1);
    if (tag.empty() || tag.back() == '/') {
      continue;
    }
    const bool closing = tag.front() == '/';
    std::string name = tag.substr(closing ? 1 : 0);
    name = name.substr(0, name.find_first_of(" \t\n"));
    if (name == "br" || name == "img") {
      continue;
    }
    if (!closing) {
      open.push_back(name);
    } else if (open.empty() || open.back() != name) {
      return false;
    } else {
      open.pop_back();
    }
  }
  return open.empty();
}

const std::string kMark = "<span class=\"search-mark\">";
const std::string kCurrent = "<span class=\"search-mark search-mark-current\">";

} // namespace

TEST(ChatSearchMarksTest, PlainTextCaseIgnored) {
  EXPECT_EQ(MarkSearchHits("<p>Meet at the Airport</p>", "airport", false),
            "<p>Meet at the " + kMark + "Airport</span></p>");
  EXPECT_EQ(MarkSearchHits("<p>tea</p>", "tea", true), "<p>" + kCurrent + "tea</span></p>");
}

TEST(ChatSearchMarksTest, Chinese) {
  EXPECT_EQ(MarkSearchHits("<p>明天下午三点开会</p>", "下午", false), "<p>明天" + kMark + "下午</span>三点开会</p>");
}

TEST(ChatSearchMarksTest, SeveralMatchesInOneText) {
  const std::string out = MarkSearchHits("<p>Tea, tea and TEA</p>", "tea", false);
  EXPECT_EQ(out, "<p>" + kMark + "Tea</span>, " + kMark + "tea</span> and " + kMark + "TEA</span></p>");
  EXPECT_TRUE(Balanced(out));
}

TEST(ChatSearchMarksTest, MatchAcrossATagIsNotMarked) {
  const std::string rml = "<p>a<b>b</b></p>";
  EXPECT_EQ(MarkSearchHits(rml, "ab", false), rml);
}

TEST(ChatSearchMarksTest, TagsAndAttributesAreNeverTouched) {
  const std::string rml = R"(<p class="tea"><img src="tea.png" alt='tea'/>see you</p>)";
  EXPECT_EQ(MarkSearchHits(rml, "tea", false), rml);
  EXPECT_EQ(MarkSearchHits(rml, "img", false), rml);
  EXPECT_EQ(MarkSearchHits(rml, "src", false), rml);
  const std::string with_text = R"(<p><img src="tea.png"/>tea</p>)";
  EXPECT_EQ(MarkSearchHits(with_text, "tea", false), R"(<p><img src="tea.png"/>)" + kMark + "tea</span></p>");
}

TEST(ChatSearchMarksTest, EntitiesStayWhole) {
  const std::string rml = "<p>salt &amp; pepper</p>";
  EXPECT_EQ(MarkSearchHits(rml, "amp", false), rml);
  EXPECT_EQ(MarkSearchHits(rml, "&", false), rml);
  EXPECT_EQ(MarkSearchHits(rml, "pepper", false), "<p>salt &amp; " + kMark + "pepper</span></p>");
  EXPECT_EQ(MarkSearchHits("<p>a &lt;b&gt; c</p>", "<b>", false), "<p>a &lt;b&gt; c</p>");
}

TEST(ChatSearchMarksTest, EmptyQueryChangesNothing) {
  const std::string rml = "<p>anything</p>";
  EXPECT_EQ(MarkSearchHits(rml, "", false), rml);
}

TEST(ChatSearchMarksTest, MarkedMarkupStaysBalanced) {
  const std::string rml =
      R"(<div class="bubble"><p>Tea <b>tea</b><br/><span class="chat-mention">@tea</span> x</p><img src="t"/></div>)";
  const std::string out = MarkSearchHits(rml, "tea", true);
  EXPECT_NE(out, rml);
  EXPECT_TRUE(Balanced(rml));
  EXPECT_TRUE(Balanced(out));
}

TEST(ChatSearchMarksTest, RowsGetTheirTextBackWhenMarksChange) {
  std::vector<MessageDisplayRow> rows = {Row("a", 1), Row("b", 2)};
  MarkSearchRows(rows, {"a"}, "a", "text");
  EXPECT_EQ(std::string(rows[0].content_rml.c_str()), "<p>" + kCurrent + "text</span> a</p>");
  EXPECT_EQ(std::string(rows[1].content_rml.c_str()), "<p>text b</p>"); // not a hit
  MarkSearchRows(rows, {"a", "b"}, "b", "text");
  EXPECT_EQ(std::string(rows[0].content_rml.c_str()), "<p>" + kMark + "text</span> a</p>");
  EXPECT_EQ(std::string(rows[1].content_rml.c_str()), "<p>" + kCurrent + "text</span> b</p>");
  for (MessageDisplayRow& row : rows) {
    ClearSearchMarks(row);
  }
  EXPECT_EQ(std::string(rows[0].content_rml.c_str()), "<p>text a</p>");
  EXPECT_EQ(std::string(rows[1].content_rml.c_str()), "<p>text b</p>");
}
