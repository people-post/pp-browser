#include "domain/media/CameraCaptureOrientation.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Expected values = SDL 3.4.16 CoreMedia's per-frame SDL_PROP_SURFACE_ROTATION_FLOAT table
// (camera/coremedia/SDL_camera_coremedia.m), read by UIInterfaceOrientation enum value:
// portrait 0°, landscape-right 90°, upside-down 180°, landscape-left 270° (display rotation CW).
TEST(IosCameraRotateCwTest, FrontCameraMatchesSdlTable) {
  EXPECT_EQ(IosCameraRotateCw(true, 0), 90);  // B51: was 270 (Android front angle) → 180° upside down
  EXPECT_EQ(IosCameraRotateCw(true, 90), 180);
  EXPECT_EQ(IosCameraRotateCw(true, 180), 270);
  EXPECT_EQ(IosCameraRotateCw(true, 270), 0);
}

TEST(IosCameraRotateCwTest, BackCameraMatchesSdlTable) {
  EXPECT_EQ(IosCameraRotateCw(false, 0), 90);
  EXPECT_EQ(IosCameraRotateCw(false, 90), 0);
  EXPECT_EQ(IosCameraRotateCw(false, 180), 270);
  EXPECT_EQ(IosCameraRotateCw(false, 270), 180);
}

TEST(IosCameraRotateCwTest, SnapsAndWrapsDisplayRotation) {
  EXPECT_EQ(IosCameraRotateCw(true, 360), 90);
  EXPECT_EQ(IosCameraRotateCw(true, -90), 0);
  EXPECT_EQ(IosCameraRotateCw(false, 88), 0);  // snaps to 90
}

}  // namespace
}  // namespace pbr
