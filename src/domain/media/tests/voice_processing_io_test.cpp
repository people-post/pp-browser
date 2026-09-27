#include "domain/media/VoiceProcessingIo.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(VoiceProcessingIoTest, ClosedByDefault) {
  VoiceProcessingIo io;
  EXPECT_FALSE(io.IsOpen());
  int16_t buf[960] = {};
  EXPECT_EQ(io.ReadCapture(buf, 960), 0u);
  EXPECT_EQ(io.WritePlayout(buf, 960), 0u);
  EXPECT_EQ(io.QueuedPlayoutBytes(), 0u);
  EXPECT_FALSE(io.TakeDeviceChanged());
  EXPECT_EQ(io.PlayoutUnderruns(), 0u);
}

TEST(VoiceProcessingIoTest, CloseIsIdempotentWithoutOpen) {
  VoiceProcessingIo io;
  io.Close();
  io.Close();
  EXPECT_FALSE(io.IsOpen());
}

}  // namespace
}  // namespace pbr
