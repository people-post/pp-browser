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

TEST(ChatOpenMetricsTest, PictureSourcesAreUnescapedInRowOrder) {
  const std::vector<MessageDisplayRow> rows = {
      Row("<div><img class=\"chat-attachment-image\" src=\"/a b/1.jpg\" width=\"10\"/></div>"),
      Row("<p>no picture</p>"),
      Row("<img class=\"chat-attachment-image\" src=\"/x&amp;y.jpg\"/>"),
  };
  const std::vector<std::string> sources = PictureBubbleSources(rows);
  ASSERT_EQ(sources.size(), 2u);
  EXPECT_EQ(sources[0], "/a b/1.jpg");
  EXPECT_EQ(sources[1], "/x&y.jpg");
}

} // namespace
} // namespace pbr
