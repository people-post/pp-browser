#include "lib/media/CameraCaptureOrientation.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Expected values = SDL 3.4.16 CoreMedia's per-frame SDL_PROP_SURFACE_ROTATION_FLOAT table
// (camera/coremedia/SDL_camera_coremedia.m), read by UIInterfaceOrientation enum value:
// portrait 0°, landscape-right 90°, upside-down 180°, landscape-left 270° (display rotation CW).
TEST(IosCameraRotateCwTest, FrontCameraMatchesSdlTable) {
  EXPECT_EQ(IosCameraRotateCw(true, 0), 90); // B51: was 270 (Android front angle) → 180° upside down
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
  EXPECT_EQ(IosCameraRotateCw(false, 88), 0); // snaps to 90
}

// Desktop (Default) build: the frame rotation stays the one resolved at open, whatever the display.
TEST(CameraFrameRotateCwTest, DefaultKeepsOpenedRotation) {
  CameraCaptureTransform opened;
  opened.rotate_cw = 180;
  opened.front_facing = true;
  EXPECT_EQ(CameraFrameRotateCw(opened, 0), 180);
  EXPECT_EQ(CameraFrameRotateCw(opened, 90), 180);
  EXPECT_EQ(CameraFrameRotateCw(opened, 270), 180);
  opened.rotate_cw = 0;
  EXPECT_EQ(CameraFrameRotateCw(opened, 90), 0);
}

TEST(CameraPreviewRotateCwTest, DefaultPreviewMatchesTheSentFrame) {
  CameraCaptureTransform opened;
  opened.rotate_cw = 90;
  EXPECT_EQ(CameraPreviewRotateCw(opened), 90);
  opened.rotate_cw = 0;
  EXPECT_EQ(CameraPreviewRotateCw(opened), 0);
}

// The preview takes the shape its rotation gives: a quarter turn is portrait, none (or a half turn) landscape.
TEST(CameraPreviewSizeTest, FollowsTheRotationNotTheEncodeShape) {
  CameraCaptureTransform opened;
  opened.encode_width = 360;
  opened.encode_height = 640; // opened in portrait
  int w = 0;
  int h = 0;
  CameraPreviewSize(opened, 90, w, h);
  EXPECT_EQ(w, 360);
  EXPECT_EQ(h, 640);
  CameraPreviewSize(opened, 0, w, h);
  EXPECT_EQ(w, 640);
  EXPECT_EQ(h, 360);
  CameraPreviewSize(opened, 180, w, h);
  EXPECT_EQ(w, 640);
  CameraPreviewSize(opened, 270, w, h);
  EXPECT_EQ(h, 640);
  opened.encode_width = 640;
  opened.encode_height = 360; // opened in landscape
  CameraPreviewSize(opened, 270, w, h);
  EXPECT_EQ(w, 360);
  EXPECT_EQ(h, 640);
}

} // namespace
} // namespace pbr
