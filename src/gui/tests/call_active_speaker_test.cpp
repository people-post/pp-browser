#include "gui/CallActiveSpeaker.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Levels = std::vector<std::pair<std::string, float>>;

/** Feeds `levels` every 20 ms for `ms`, from `now`; returns the speaker at the end. */
std::string Feed(CallActiveSpeaker& speaker, const Levels& levels, int64_t& now, const int64_t ms) {
  for (int64_t t = 0; t < ms; t += 20) {
    speaker.Update(levels, now += 20);
  }
  return speaker.Current();
}

// V059: a peer that speaks for a moment takes the group's spotlight; a cough does not.
TEST(CallActiveSpeakerTest, SpeakingForAMomentTakesTheSpotlightAndACoughDoesNot) {
  CallActiveSpeaker speaker;
  int64_t now = 1000;
  EXPECT_EQ(Feed(speaker, {{"22", 0.3f}, {"33", 0.01f}}, now, 200), "") << "200 ms is a cough";
  EXPECT_EQ(Feed(speaker, {{"22", 0.01f}, {"33", 0.01f}}, now, 100), "");
  EXPECT_EQ(Feed(speaker, {{"22", 0.01f}, {"33", 0.3f}}, now, 800), "33");
  EXPECT_EQ(Feed(speaker, {{"22", 0.01f}, {"33", 0.01f}}, now, 5000), "33") << "quiet: the last speaker stays";
}

// The spotlight holds: no switch while the spotlit one still talks, nor within kHoldMs.
TEST(CallActiveSpeakerTest, TheSpotlightDoesNotFlickerBetweenTwoTalking) {
  CallActiveSpeaker speaker;
  int64_t now = 1000;
  ASSERT_EQ(Feed(speaker, {{"22", 0.2f}}, now, 800), "22");
  EXPECT_EQ(Feed(speaker, {{"22", 0.2f}, {"33", 0.5f}}, now, 5000), "22") << "both talk, 33 louder: 22 keeps it";
  EXPECT_EQ(Feed(speaker, {{"22", 0.01f}, {"33", 0.5f}}, now, 1000), "33") << "22 went quiet, 33 speaks";
  EXPECT_EQ(Feed(speaker, {{"22", 0.5f}, {"33", 0.01f}}, now, 1000), "33") << "within the hold";
  EXPECT_EQ(Feed(speaker, {{"22", 0.5f}, {"33", 0.01f}}, now, 3000), "22") << "after the hold";
}

} // namespace
} // namespace pbr
