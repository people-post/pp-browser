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
      Row("<div class=\"chat-attachment-clip\"><img class=\"chat-attachment-image\" src=\"/a.jpg\" "
          "width=\"10\"/></div>"),
      Row("<div class=\"chat-attachment-clip\"><img class=\"chat-attachment-video-poster\" src=\"/p.jpg\"/></div>"),
      Row("<div class=\"chat-attachment-clip chat-pending-picture-clip\">"
          "<img class=\"chat-attachment-image\" src=\"/t.jpg\"/></div>"),
  };
  EXPECT_EQ(CountPictureBubbleRows(rows), 2u);
  EXPECT_FALSE(IsPictureBubbleRow(rows[0]));
  EXPECT_FALSE(IsPictureBubbleRow(rows[2]));
  EXPECT_EQ(CountPictureBubbleRows({}), 0u);
}

// B203: a placeholder row is a picture row too, and counted on its own (chat.open's `placeholders`).
TEST(ChatOpenMetricsTest, PlaceholdersArePictureRowsAndCountedApart) {
  const std::vector<MessageDisplayRow> rows = {
      Row("<div class=\"chat-attachment-clip chat-picture-placeholder\" style=\"width:10dp;height:10dp\" "
          "pic-id=\"m1\" pic-hash=\"aa\"></div>"),
      Row("<div class=\"chat-attachment-clip\" pic-id=\"m2\" pic-hash=\"bb\"><img class=\"chat-attachment-image\" "
          "src=\"/b.jpg\"/></div>"),
      Row("<div class=\"chat-attachment-clip chat-picture-placeholder\" style=\"width:10dp;height:10dp\" "
          "pic-id=\"m3\" pic-hash=\"aa\"></div>"),
      Row("<p class=\"text\">hello</p>"),
  };
  EXPECT_EQ(CountPictureBubbleRows(rows), 3u);
  EXPECT_EQ(CountPicturePlaceholderRows(rows), 2u);
  EXPECT_EQ(PicturePlaceholderHashes(rows), (std::vector<std::string>{"aa"})) << "one file, asked for once";
}

TEST(ChatOpenMetricsTest, PreviewsProbeReportsWhenTheOpensPlaceholdersAreAllDrawn) {
  using Clock = ChatPreviewsProbe::Clock;
  const Clock::time_point t0 = Clock::now();
  ChatPreviewsProbe probe;
  probe.Start({"a", "b", "c"}, t0, /*slow_frames_now=*/10);
  ASSERT_TRUE(probe.Active());
  probe.SetVisible({"b", "c", "zz"}, t0 + std::chrono::milliseconds(50));
  EXPECT_FALSE(probe.Done("b", false, t0 + std::chrono::milliseconds(300)));
  EXPECT_FALSE(probe.Done("x", false, t0 + std::chrono::milliseconds(310))) << "not one of the open's";
  EXPECT_FALSE(probe.Done("c", true, t0 + std::chrono::milliseconds(400)));
  EXPECT_TRUE(probe.Done("a", false, t0 + std::chrono::milliseconds(900)));
  const ChatPreviewsProbe::Report report = probe.Finish(t0 + std::chrono::milliseconds(900), 12);
  EXPECT_FALSE(probe.Active());
  EXPECT_EQ(report.count, 3);
  EXPECT_EQ(report.failed, 1);
  EXPECT_EQ(report.ms, 900);
  EXPECT_EQ(report.visible_ms, 400) << "the two on screen";
  EXPECT_EQ(report.slow_frames, 2);
  EXPECT_TRUE(report.complete);
}

TEST(ChatPreviewsProbe, LeftBeforeTheyCameIsIncompleteAndNothingToWaitForStartsNothing) {
  using Clock = ChatPreviewsProbe::Clock;
  const Clock::time_point t0 = Clock::now();
  ChatPreviewsProbe probe;
  probe.Start({}, t0, 0);
  EXPECT_FALSE(probe.Active()) << "every picture had its preview: no line";
  probe.Start({"a", "b"}, t0, 0);
  EXPECT_FALSE(probe.Done("a", false, t0 + std::chrono::milliseconds(100)));
  const ChatPreviewsProbe::Report report = probe.Finish(t0 + std::chrono::milliseconds(200), 0);
  EXPECT_FALSE(report.complete);
  EXPECT_EQ(report.visible_ms, -1);
  EXPECT_EQ(report.ms, 200);
}

} // namespace
} // namespace pbr
