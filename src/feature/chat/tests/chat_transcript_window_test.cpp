#include "common/chat/MessagingLimits.h"
#include "base/ui/ChatWidgetTypes.h"
#include "feature/chat/ChatTranscriptScroller.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

pbr::MessageDisplayRow MakeRow(int64_t order) {
  pbr::MessageDisplayRow row;
  row.message_id = ("m" + std::to_string(order)).c_str();
  row.display_order = order;
  row.content_rml = "x";
  row.has_content = true;
  return row;
}

std::vector<pbr::MessageDisplayRow> MakeRows(int64_t first, size_t count) {
  std::vector<pbr::MessageDisplayRow> rows;
  rows.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    rows.push_back(MakeRow(first + static_cast<int64_t>(i)));
  }
  return rows;
}

} // namespace

TEST(ChatTranscriptWindowTest, TrimWhilePinnedDropsOldest) {
  auto rows = MakeRows(1, pbr::kMaxMessagesDomWindow + 50);
  std::optional<int64_t> loaded_min = 1;
  std::optional<int64_t> loaded_max;
  bool has_more = false;
  EXPECT_TRUE(pbr::ChatTranscriptScroller::TrimDomWindow(rows, /*pinned_to_bottom=*/true, loaded_min,
                                                         loaded_max, has_more));
  EXPECT_EQ(rows.size(), pbr::kMaxMessagesDomWindow);
  EXPECT_EQ(rows.front().display_order, 51);
  EXPECT_EQ(*loaded_min, 51);
  EXPECT_FALSE(loaded_max.has_value());
  EXPECT_TRUE(has_more);
}

TEST(ChatTranscriptWindowTest, TrimWhileReadingHistoryDropsNewest) {
  auto rows = MakeRows(1, pbr::kMaxMessagesDomWindow + 25);
  std::optional<int64_t> loaded_min = 1;
  std::optional<int64_t> loaded_max;
  bool has_more = true;
  EXPECT_TRUE(pbr::ChatTranscriptScroller::TrimDomWindow(rows, /*pinned_to_bottom=*/false, loaded_min,
                                                         loaded_max, has_more));
  EXPECT_EQ(rows.size(), pbr::kMaxMessagesDomWindow);
  EXPECT_EQ(rows.front().display_order, 1);
  EXPECT_EQ(rows.back().display_order, static_cast<int64_t>(pbr::kMaxMessagesDomWindow));
  EXPECT_EQ(*loaded_min, 1);
  ASSERT_TRUE(loaded_max.has_value());
  EXPECT_EQ(*loaded_max, static_cast<int64_t>(pbr::kMaxMessagesDomWindow));
}

TEST(ChatTranscriptWindowTest, UnderCapIsNoOp) {
  auto rows = MakeRows(1, 10);
  std::optional<int64_t> loaded_min = 1;
  std::optional<int64_t> loaded_max;
  bool has_more = false;
  EXPECT_FALSE(pbr::ChatTranscriptScroller::TrimDomWindow(rows, true, loaded_min, loaded_max, has_more));
  EXPECT_EQ(rows.size(), 10U);
}
