// The reactions row of a message's sheet (Kenneth 2026-11-04, PLAN-ui-refresh-impl 62.7): the user's last-used emoji
// first, then the defaults, no repeats, at most kQuickReactionCount.
#include "feature/presenters/QuickReactions.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(QuickReactionsTest, WithNothingUsedItIsTheSixDefaults) {
  EXPECT_EQ(QuickReactions({}), DefaultQuickReactions());
  EXPECT_EQ(DefaultQuickReactions().size(), 6u);
}

TEST(QuickReactionsTest, TheLastUsedComeFirstThenTheDefaultsWithoutRepeats) {
  // "❤" without its variation selector is the same reaction as the default "❤️".
  const std::vector<std::string> row = QuickReactions({"🎉", "\xE2\x9D\xA4", "🔥"});
  const std::vector<std::string> expected = {"🎉", "\xE2\x9D\xA4", "🔥", "👍", "😂", "😮", "😢", "🙏"};
  EXPECT_EQ(row, expected);
}

TEST(QuickReactionsTest, ALongHistoryFillsTheRowAndStopsThere) {
  std::vector<std::string> recent;
  for (const char* emoji : {"😀", "😃", "😄", "😁", "😆", "😅", "🤣", "🙂", "🙃", "😉", "😊", "😇", "🥰", "😍"}) {
    recent.push_back(emoji);
  }
  const std::vector<std::string> row = QuickReactions(recent);
  ASSERT_EQ(row.size(), kQuickReactionCount);
  EXPECT_EQ(row.front(), "😀");
  EXPECT_EQ(row.back(), "😇");
  EXPECT_EQ(QuickReactions({"", "  "}), DefaultQuickReactions()) << "blank entries are skipped";
}

} // namespace
} // namespace pbr
