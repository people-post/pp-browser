// Which finger events the avatar crop page keeps from the UI library (PLAN-avatar-edit items 9 and 12): a second
// finger must not click or scroll the page, and the buttons still work when nothing is being dragged.
#include "feature/presenters/CropFingerOwnership.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using K = FingerEventKind;
constexpr CropFingerRoute kOwn = CropFingerRoute::Own;
constexpr CropFingerRoute kPass = CropFingerRoute::Pass;
constexpr bool kInside = true;
constexpr bool kOutside = false;

TEST(CropFingerOwnershipTest, ADownInsideThePictureIsOwnedWithItsMovesAndUp) {
  CropFingerOwnership o;
  EXPECT_EQ(o.OnFinger(K::Down, 1, kInside), kOwn);
  EXPECT_TRUE(o.Tracking());
  EXPECT_EQ(o.OnFinger(K::Move, 1, kOutside), kOwn) << "dragged off the picture onto a button";
  EXPECT_EQ(o.OnFinger(K::Up, 1, kOutside), kOwn);
  EXPECT_FALSE(o.Tracking());
}

TEST(CropFingerOwnershipTest, ADownOutsideWithNothingTrackedPasses) {
  CropFingerOwnership o;
  EXPECT_EQ(o.OnFinger(K::Down, 1, kOutside), kPass) << "the Done button";
  EXPECT_FALSE(o.Tracking());
  EXPECT_EQ(o.OnFinger(K::Move, 1, kInside), kPass) << "its moves go to the UI with it";
  EXPECT_EQ(o.OnFinger(K::Up, 1, kInside), kPass);
}

TEST(CropFingerOwnershipTest, WhileAFingerIsTrackedADownAnywhereIsOwned) {
  CropFingerOwnership o;
  ASSERT_EQ(o.OnFinger(K::Down, 1, kInside), kOwn);
  EXPECT_EQ(o.OnFinger(K::Down, 2, kOutside), kOwn) << "a finger brushing the bottom bar during a drag";
  EXPECT_EQ(o.OnFinger(K::Down, 3, kOutside), kOwn) << "a third finger is owned too, never half in the UI";
  EXPECT_EQ(o.OnFinger(K::Up, 1, kInside), kOwn);
  EXPECT_EQ(o.OnFinger(K::Move, 2, kOutside), kOwn);
  EXPECT_EQ(o.OnFinger(K::Up, 2, kOutside), kOwn);
  EXPECT_EQ(o.OnFinger(K::Up, 3, kOutside), kOwn);
  EXPECT_FALSE(o.Tracking());
  EXPECT_EQ(o.OnFinger(K::Down, 4, kOutside), kPass) << "all lifted: the buttons work again";
}

TEST(CropFingerOwnershipTest, CancelAndFocusLossForgetEveryFinger) {
  CropFingerOwnership o;
  o.OnFinger(K::Down, 1, kInside);
  o.OnFinger(K::Down, 2, kInside);
  EXPECT_EQ(o.OnFinger(K::Cancel, 1, kInside), kOwn) << "its Down never reached the UI";
  EXPECT_FALSE(o.Tracking());
  EXPECT_EQ(o.OnFinger(K::Move, 2, kInside), kPass) << "forgotten";
  EXPECT_EQ(o.OnFinger(K::Down, 5, kOutside), kPass);

  o.OnFinger(K::Down, 1, kInside);
  o.Clear(); // window focus lost / app to the background / page closed
  EXPECT_FALSE(o.Tracking());
  EXPECT_EQ(o.OnFinger(K::Down, 2, kOutside), kPass);
  EXPECT_EQ(o.OnFinger(K::Cancel, 9, kInside), kPass) << "an unknown finger's cancel is the UI's";
}

TEST(CropFingerOwnershipTest, PausedPassesEverything) {
  CropFingerOwnership o;
  o.OnFinger(K::Down, 1, kInside);
  o.SetPaused(true); // the PIN pad opened over the page
  EXPECT_TRUE(o.paused());
  EXPECT_FALSE(o.Tracking()) << "pausing forgets the fingers";
  EXPECT_EQ(o.OnFinger(K::Down, 2, kInside), kPass) << "a PIN key over the picture area";
  EXPECT_EQ(o.OnFinger(K::Move, 1, kInside), kPass);
  EXPECT_EQ(o.OnFinger(K::Up, 2, kInside), kPass);
  EXPECT_FALSE(o.Tracking());
  o.SetPaused(false);
  EXPECT_EQ(o.OnFinger(K::Down, 3, kInside), kOwn) << "back on the crop page";
}

} // namespace
} // namespace pbr
