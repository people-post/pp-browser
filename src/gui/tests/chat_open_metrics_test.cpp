#include "gui/chat/ChatOpenMetrics.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

MessageDisplayRow Row(const char* rml) {
  MessageDisplayRow row;
  row.content_rml = rml;
  return row;
}

TEST(ChatOpenMetricsTest, CountsPictureBubbleRowsOnly) {
  const std::vector<MessageDisplayRow> rows = {
      Row("<p class=\"text\">hello</p>"),
      Row("<div class=\"chat-attachment-clip\"><img class=\"chat-attachment-image\" src=\"/a.jpg\" width=\"10\"/></div>"),
      Row("<div class=\"chat-attachment-clip\"><img class=\"chat-attachment-video-poster\" src=\"/p.jpg\"/></div>"),
      Row("<div class=\"chat-attachment-clip chat-pending-picture-clip\">"
          "<img class=\"chat-attachment-image\" src=\"/t.jpg\"/></div>"),
  };
  EXPECT_EQ(CountPictureBubbleRows(rows), 2u);
  EXPECT_FALSE(IsPictureBubbleRow(rows[0]));
  EXPECT_FALSE(IsPictureBubbleRow(rows[2]));
  EXPECT_EQ(CountPictureBubbleRows({}), 0u);
}

} // namespace
} // namespace pbr
