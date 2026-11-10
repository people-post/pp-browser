// Which finger events the avatar crop page takes before the UI library sees them (PLAN-avatar-edit items 9 and 12),
// driven through SdlAppEvents::PreProcess with fake SDL events.
#include "foundation/platform/ui/SdlAppEvents.h"
#include "gui/avatar/AvatarCropTouch.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// A 1000 x 2000 px window at 2 px per dp: 500 x 1000 dp. The crop element is x 0..500, y 100..800 dp.
CropTouchSpace Space() {
  CropTouchSpace s;
  s.window_w_px = 1000.0f;
  s.window_h_px = 2000.0f;
  s.px_per_dp = 2.0f;
  s.box_x = 0.0f;
  s.box_y = 100.0f;
  s.box_w = 500.0f;
  s.box_h = 700.0f;
  return s;
}

SDL_Event Finger(const Uint32 type, const uint64_t id, const float x_dp, const float y_dp) {
  SDL_Event e;
  SDL_zero(e);
  e.type = type;
  e.tfinger.touchID = 77;
  e.tfinger.fingerID = id;
  e.tfinger.x = x_dp * 2.0f / 1000.0f;
  e.tfinger.y = y_dp * 2.0f / 2000.0f;
  return e;
}

class CropTouchRoutingTest : public ::testing::Test {
protected:
  void SetUp() override {
    SdlAppEvents::SetFingerHandler([this](ui::Context*, const SDL_Event& e) { return touch.OnSdlEvent(e, Space()); });
  }
  void TearDown() override { SdlAppEvents::SetFingerHandler({}); }

  // True when PreProcess swallowed the event.
  bool Swallowed(SDL_Event e) {
    bool propagate = true;
    return SdlAppEvents::PreProcess(nullptr, e, propagate);
  }
  AvatarCropTouch touch;
};

TEST_F(CropTouchRoutingTest, ADownInsideTheElementIsOwnedAndItsMotionAndUpAreSwallowed) {
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 400)));
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 1, 260, 410)));
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_UP, 1, 260, 410)));
  EXPECT_FALSE(touch.Tracking());
}

TEST_F(CropTouchRoutingTest, ADownOutsideWithNothingTrackedPassesSoTheButtonsWork) {
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 900))); // the bottom bar
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 1, 250, 910)));
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_UP, 1, 250, 910)));
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 2, 250, 50))); // the title bar
}

TEST_F(CropTouchRoutingTest, WhileOneFingerIsTrackedANewDownIsSwallowedWhereverItLands) {
  ASSERT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 400)));
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 2, 250, 900))); // on the bottom bar
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 2, 250, 905)));
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 3, 100, 50))); // a third finger: owned, ignored by the gesture
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_UP, 3, 100, 50)));
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_UP, 2, 250, 905)));
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_UP, 1, 250, 400)));
  EXPECT_FALSE(touch.Tracking());
  // Everything lifted: a down on the bar passes again.
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 4, 250, 900)));
}

TEST_F(CropTouchRoutingTest, CanceledAndTheWindowLosingFocusForgetEveryFinger) {
  ASSERT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 400)));
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_CANCELED, 1, 250, 400)));
  EXPECT_FALSE(touch.Tracking());

  ASSERT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 400)));
  SDL_Event lost;
  SDL_zero(lost);
  lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  // Straight to the touch (through PreProcess these events also move the app's global foreground / focus state).
  EXPECT_FALSE(touch.OnSdlEvent(lost, Space())); // the event itself still goes on
  EXPECT_FALSE(touch.Tracking());
  // The finger that was down is unknown now: its motion passes, nothing jumps.
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 1, 300, 420)));

  ASSERT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 5, 250, 400)));
  SDL_Event background;
  SDL_zero(background);
  background.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
  EXPECT_FALSE(touch.OnSdlEvent(background, Space()));
  EXPECT_FALSE(touch.Tracking());
}

TEST_F(CropTouchRoutingTest, WhilePausedEverythingPassesAndPausingForgetsTheFingers) {
  ASSERT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 400)));
  touch.SetPaused(true);
  EXPECT_FALSE(touch.Tracking());
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 1, 255, 400)));
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 2, 250, 400)));
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_UP, 2, 250, 400)));
  touch.SetPaused(false);
  EXPECT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 3, 250, 400)));
}

TEST_F(CropTouchRoutingTest, WithNoHandlerSetFingersAreNotTouched) {
  SdlAppEvents::SetFingerHandler({});
  EXPECT_FALSE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 400)));
  EXPECT_FALSE(touch.Tracking());
}

TEST_F(CropTouchRoutingTest, OneFingerDragPansOnceWhenAppliedAndTwoFingersSumToOneUpdate) {
  AvatarCropFrame frame(1000, 500, 300);
  frame.Pinch({0, 0}, 3.0, 0.0); // room to move
  const AvatarCropFrame::Point before = frame.centre();
  ASSERT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 450)));
  Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 1, 260, 450));
  Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 1, 270, 450));
  EXPECT_TRUE(touch.Apply(frame));
  EXPECT_NE(frame.centre().x, before.x);
  EXPECT_FALSE(touch.Apply(frame)) << "what was summed is applied once";
}

TEST_F(CropTouchRoutingTest, ATwoFingerTwistAtScaleOneLeavesTheScaleAlone) {
  AvatarCropFrame frame(400, 300, 300);
  // Fingers 100 dp apart about the element's centre (250, 450), then turned 30 degrees, one finger per event.
  ASSERT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 200, 450)));
  ASSERT_TRUE(Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 2, 300, 450)));
  const float c = 0.8660254f * 50.0f;
  const float s = 0.5f * 50.0f;
  Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 1, 250 - c, 450 - s));
  Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 2, 250 + c, 450 + s));
  EXPECT_TRUE(touch.Apply(frame));
  EXPECT_NEAR(frame.scale(), 1.0, 1e-4);
  EXPECT_NEAR(frame.theta(), 0.5235988, 1e-3);
}

// "Done" reads the crop after the movement summed this frame was applied; pausing the touch first would drop it.
TEST_F(CropTouchRoutingTest, ApplyingBeforePausingKeepsTheLastMovementAndPausingFirstDropsIt) {
  const auto drag = [&](AvatarCropFrame& frame, const bool apply_first) -> double {
    // Returns the centre before the drag.
    touch.Clear();
    touch.SetPaused(false);
    frame.Pinch({0, 0}, 3.0, 0.0);
    const double before = frame.centre().x;
    Swallowed(Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 450));
    Swallowed(Finger(SDL_EVENT_FINGER_MOTION, 1, 270, 450));
    if (apply_first) {
      touch.Apply(frame);
      touch.SetPaused(true);
    } else {
      touch.SetPaused(true);
      touch.Apply(frame);
    }
    return before;
  };
  AvatarCropFrame flushed(1000, 500, 300);
  const double flushed_before = drag(flushed, true);
  EXPECT_NE(flushed.centre().x, flushed_before) << "flush, then pause: the movement is in the frame";
  AvatarCropFrame dropped(1000, 500, 300);
  const double dropped_before = drag(dropped, false);
  EXPECT_EQ(dropped.centre().x, dropped_before) << "pause, then flush: the movement is lost";
}

TEST_F(CropTouchRoutingTest, AFingerFromATrackpadIsNotATouchOnThePicture) {
  // A fake device id is taken as a direct touch screen (this is the routing of the cases above); the check itself
  // is SDL_GetTouchDeviceType, which only a real indirect device answers with INDIRECT_* (and only once video is up).
  SDL_Event e = Finger(SDL_EVENT_FINGER_DOWN, 1, 250, 400);
  EXPECT_TRUE(Swallowed(e));
}

} // namespace
} // namespace pbr
