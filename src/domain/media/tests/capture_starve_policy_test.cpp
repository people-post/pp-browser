#include "domain/media/CaptureStarvePolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using P = CaptureStarvePolicy;

// An SDL mic is reopened as soon as it goes silent (wedged Android AudioRecord), at any age.
TEST(CaptureStarvePolicyTest, SdlDeviceReopensAfterHalfASecond) {
  EXPECT_EQ(P::ReopenAfterMs(false, 0), P::kSilenceAfterMs);
  EXPECT_EQ(P::ReopenAfterMs(false, 60'000), P::kSilenceAfterMs);
}

// B56: voice processing that has not warmed up is not "starved" at 500 ms — reopening it then
// restarted it in a loop for up to a minute.
TEST(CaptureStarvePolicyTest, VoiceProcessingGetsAWarmUpAfterEachOpen) {
  EXPECT_EQ(P::ReopenAfterMs(true, 0), P::kVoiceWarmupMs);
  EXPECT_EQ(P::ReopenAfterMs(true, 700), P::kVoiceWarmupMs) << "the 2026-09-29 loop reopened here";
  EXPECT_EQ(P::ReopenAfterMs(true, P::kVoiceWarmupMs - 1), P::kVoiceWarmupMs);
}

TEST(CaptureStarvePolicyTest, WarmVoiceProcessingStallsAfterOneSecond) {
  EXPECT_EQ(P::ReopenAfterMs(true, P::kVoiceWarmupMs), P::kVoiceStallMs);
  EXPECT_EQ(P::ReopenAfterMs(true, 60'000), P::kVoiceStallMs);
  EXPECT_GT(P::kVoiceStallMs, P::kSilenceAfterMs);
}

// PCM right after a reopen does not clear the run of starvation reopens (else the fallback to SDL
// never triggered); only a unit that kept running does.
TEST(CaptureStarvePolicyTest, OnlyAUnitThatKeptRunningEndsTheReopenRun) {
  EXPECT_FALSE(P::HealthyAfterOpen(0));
  EXPECT_FALSE(P::HealthyAfterOpen(P::kVoiceHealthyMs - 1));
  EXPECT_TRUE(P::HealthyAfterOpen(P::kVoiceHealthyMs));
}

} // namespace
} // namespace pbr
