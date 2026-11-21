// The wide layout's hover bar shows a pencil only on a row whose message the menu would offer Edit for: the row's
// can_edit follows TranscriptFacts::can_edit, and never shows on a row with no actions at all (a system line).
#include "feature/conversations/InboxController.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/TranscriptRml.h"
#include "lib/messaging/TranscriptAssembly.h"

#include <gtest/gtest.h>

#include <set>

namespace pbr {
namespace {

TranscriptRow Row(const std::string& id, const std::string& sender, const ChatContentType type) {
  TranscriptRow row;
  row.message.id = id;
  row.message.thread_id = "t";
  row.message.sender_contact_id = sender;
  row.message.content_type = type;
  row.message.text = "hi";
  row.message.timestamp = 1;
  return row;
}

class TranscriptCanEditTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
};

TEST_F(TranscriptCanEditTest, TheRowCarriesTheFactsAnswerOnlyWhereTheRowHasActions) {
  const std::vector<TranscriptRow> rows_in = {Row("own", kLocalSelfContactId, ChatContentType::Text),
                                              Row("theirs", "peer", ChatContentType::Text),
                                              Row("system", kLocalSelfContactId, ChatContentType::System)};
  TranscriptFacts facts;
  // The app's answer is true for our own text inside the edit window; here the test says "own" and "system" qualify.
  facts.can_edit = [](const std::string& id) { return id == "own" || id == "system"; };
  const auto rows = RenderTranscript(rows_in, facts);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_TRUE(rows[0].can_edit) << "an own text message the menu offers Edit for";
  EXPECT_FALSE(rows[1].can_edit) << "a received message";
  EXPECT_FALSE(rows[2].can_edit) << "a system line has no actions, so no pencil whatever the answer";
  EXPECT_FALSE(rows[2].has_actions);
}

TEST_F(TranscriptCanEditTest, NoAnswerMeansNoPencil) {
  const auto rows = RenderTranscript({Row("own", kLocalSelfContactId, ChatContentType::Text)}, TranscriptFacts{});
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(rows[0].has_actions);
  EXPECT_FALSE(rows[0].can_edit);
}

} // namespace
} // namespace pbr
