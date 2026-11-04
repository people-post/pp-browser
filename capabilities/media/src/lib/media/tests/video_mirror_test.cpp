#include "lib/media/VideoYuv.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace pbr {
namespace {

VideoFrameRgba Row(std::vector<uint8_t> first_channel) {
  VideoFrameRgba frame;
  frame.width = static_cast<int>(first_channel.size());
  frame.height = 1;
  for (const uint8_t v : first_channel) {
    frame.rgba.insert(frame.rgba.end(), {v, 0, 0, 255});
  }
  return frame;
}

std::vector<uint8_t> FirstChannel(const VideoFrameRgba& frame) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < frame.rgba.size(); i += 4) {
    out.push_back(frame.rgba[i]);
  }
  return out;
}

// The self-view is drawn as a mirror (Kenneth 2026-11-03 "视频显示自己是反的").
TEST(VideoMirrorTest, EachRowIsFlippedLeftToRight) {
  VideoFrameRgba odd = Row({1, 2, 3});
  MirrorRgbaInPlace(odd);
  EXPECT_EQ(FirstChannel(odd), (std::vector<uint8_t>{3, 2, 1}));

  VideoFrameRgba even = Row({1, 2, 3, 4});
  MirrorRgbaInPlace(even);
  EXPECT_EQ(FirstChannel(even), (std::vector<uint8_t>{4, 3, 2, 1}));
}

TEST(VideoMirrorTest, RowsStayInPlaceAndPixelsKeepTheirChannels) {
  VideoFrameRgba frame;
  frame.width = 2;
  frame.height = 2;
  frame.rgba = {1, 2, 3, 4, 5, 6, 7, 8,  // row 0
                9, 10, 11, 12, 13, 14, 15, 16};  // row 1
  MirrorRgbaInPlace(frame);
  EXPECT_EQ(frame.rgba, (std::vector<uint8_t>{5, 6, 7, 8, 1, 2, 3, 4, 13, 14, 15, 16, 9, 10, 11, 12}));
}

TEST(VideoMirrorTest, AShortBufferIsLeftAlone) {
  VideoFrameRgba frame;
  frame.width = 4;
  frame.height = 1;
  frame.rgba = {1, 2, 3, 4};
  MirrorRgbaInPlace(frame);
  EXPECT_EQ(frame.rgba, (std::vector<uint8_t>{1, 2, 3, 4}));
}

} // namespace
} // namespace pbr
