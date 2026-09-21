#include "common/ui/WorkingSetCodec.h"

#include <gtest/gtest.h>

namespace {

using namespace pbr;

TEST(WorkingSetCodecTest, RoundTripCandidates) {
  WorkingSetCandidate candidate;
  candidate.block_index = 2;
  candidate.kind = WorkingSetKind::LongList;
  candidate.affinity = WorkingSetAffinity::Feed;
  candidate.auto_open = true;
  candidate.title = "Search results";
  candidate.subtitle = "3 items";
  candidate.artifact_rml = "<div class=\"working-set-long-list\">x</div>";
  candidate.teaser_rml =
      "<button class=\"chat-working-set-chip\" data-event-click=\"open_working_set('e1', 2)\">View</button>";

  const std::string json = WorkingSetCandidatesToJson({candidate});
  const auto restored = WorkingSetCandidatesFromJson(json);
  ASSERT_EQ(restored.size(), 1u);
  EXPECT_EQ(restored[0].block_index, 2);
  EXPECT_EQ(restored[0].kind, WorkingSetKind::LongList);
  EXPECT_EQ(restored[0].affinity, WorkingSetAffinity::Feed);
  EXPECT_TRUE(restored[0].auto_open);
  EXPECT_EQ(restored[0].title, "Search results");
  EXPECT_EQ(restored[0].artifact_rml, candidate.artifact_rml);
}

TEST(WorkingSetCodecTest, MarksUnavailableChips) {
  const std::string rml =
      "<div class=\"stack\"><p>Found 2</p>"
      "<button class=\"chat-working-set-chip\" data-event-click=\"open_working_set('abc', 1)\">"
      "View in panel (2 items)</button></div>";
  EXPECT_TRUE(ContentRmlHasActiveWorkingSetChip(rml));
  const std::string marked = MarkWorkingSetChipsUnavailable(rml);
  EXPECT_NE(marked.find("chat-working-set-chip-disabled"), std::string::npos);
  EXPECT_NE(marked.find("Results no longer available"), std::string::npos);
  EXPECT_EQ(marked.find("open_working_set"), std::string::npos);
}

TEST(WorkingSetCodecTest, StripsInlinedPanelActionSuggestions) {
  const std::string rml =
      "<div class=\"stack\"><p>Found 2</p>"
      "<button class=\"chat-working-set-chip\" data-event-click=\"open_working_set('abc', 1)\">"
      "View in panel (2 items)</button>"
      "<button class=\"chat-suggestion\" data-event-click=\"send_chat_action('abc', 0)\">Add contact</button>"
      "<button class=\"chat-suggestion\" data-event-click=\"send_chat_action('abc', 1)\">Message</button>"
      "</div>";
  const std::string cleaned = StripInlinedWorkingSetActionSuggestions(rml);
  EXPECT_NE(cleaned.find("open_working_set"), std::string::npos);
  EXPECT_EQ(cleaned.find("Add contact"), std::string::npos);
  EXPECT_EQ(cleaned.find("chat-suggestion"), std::string::npos);
}

}  // namespace
