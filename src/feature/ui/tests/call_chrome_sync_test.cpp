#include "feature/ui/CallChromeSync.h"

#include <gtest/gtest.h>

TEST(CallChromeSyncTest, IdlePollUnchangedIsNone) {
  pbr::CallChromeLayer layer;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(layer, layer), pbr::CallChromeUpdate::None);
}

TEST(CallChromeSyncTest, RingAppearRemounts) {
  pbr::CallChromeLayer synced;
  pbr::CallChromeLayer next;
  next.ring_active = true;
  next.ring_call_id = "c1";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, RingDisappearRemounts) {
  pbr::CallChromeLayer synced;
  synced.ring_active = true;
  synced.ring_call_id = "c1";
  pbr::CallChromeLayer next;
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}

TEST(CallChromeSyncTest, SubtitleOnlyDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_subtitle = "Connecting…";
  pbr::CallChromeLayer next = synced;
  next.in_call_subtitle = "Connected";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, MicLevelOnlyDirties) {
  pbr::CallChromeLayer synced;
  synced.in_call_active = true;
  synced.in_call_id = "c1";
  synced.in_call_mic_level = 0;
  synced.in_call_mic_hint = "Silent";
  pbr::CallChromeLayer next = synced;
  next.in_call_mic_level = 3;
  next.in_call_mic_hint = "Speaking";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::DirtyOnly);
}

TEST(CallChromeSyncTest, SwitchCallIdRemounts) {
  pbr::CallChromeLayer synced;
  synced.ring_active = true;
  synced.ring_call_id = "c1";
  pbr::CallChromeLayer next = synced;
  next.ring_call_id = "c2";
  EXPECT_EQ(pbr::ClassifyCallChromeUpdate(synced, next), pbr::CallChromeUpdate::Remount);
}
