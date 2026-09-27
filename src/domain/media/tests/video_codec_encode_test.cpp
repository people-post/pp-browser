#include "domain/media/IVideoCodec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace pbr {
namespace {

VideoFrameI420 GreyFrame(int width, int height) {
  VideoFrameI420 frame;
  frame.width = width;
  frame.height = height;
  frame.y.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0x80);
  frame.u.assign(static_cast<size_t>(width / 2) * static_cast<size_t>(height / 2), 0x80);
  frame.v.assign(frame.u.size(), 0x80);
  return frame;
}

// Encode sizes are rarely 16-aligned (640×360 → 640×368 surface). Linux VA-API staged such frames
// in a frame-sized image while radeonsi read the whole surface: heap overflow in vaPutImage (ASan).
TEST(VideoCodecEncodeTest, NonAlignedFramesEncodeWithinBounds) {
  auto codec = CreatePlatformVideoCodec();
  if (!codec || !codec->EncoderSupported()) {
    GTEST_SKIP() << "no hardware H264 encoder on this host";
  }
  for (const auto& [w, h] : std::vector<std::pair<int, int>>{{640, 360}, {360, 640}}) {
    auto configured = codec->ConfigureEncoder(w, h, 20);
    if (!configured) {
      GTEST_SKIP() << "encoder configure failed: " << configured.error().message;
    }
    const VideoFrameI420 frame = GreyFrame(w, h);
    // Every encode stays in bounds and succeeds; output may lag input while the encoder buffers
    // (Windows MF soft H264), so require bytes within a short run, not from every call.
    bool produced = false;
    for (int i = 0; i < 30; ++i) {
      auto encoded = codec->Encode(frame, i == 0);
      ASSERT_TRUE(encoded) << w << "x" << h << ": " << encoded.error().message;
      produced |= !encoded->annex_b.empty();
    }
    EXPECT_TRUE(produced) << w << "x" << h << ": no access unit after 30 frames";
    codec->ResetEncoder();
  }
}

} // namespace
} // namespace pbr
