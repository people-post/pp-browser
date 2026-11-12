#include "feature/presenters/CallRingerOutput.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int kSilent = DeviceRingerReading::kModeSilent;
constexpr int kVibrate = DeviceRingerReading::kModeVibrate;
constexpr int kNormal = DeviceRingerReading::kModeNormal;
constexpr int kAll = DeviceRingerReading::kFilterAll;
// NotificationManager's other filters: PRIORITY 2, NONE 3, ALARMS 4; UNKNOWN 0.
constexpr int kPriority = 2;
constexpr int kNone = 3;
constexpr int kAlarms = 4;
constexpr int kUnknownFilter = 0;

DeviceRingerReading Reading(const int mode, const int filter, const int volume) {
  return {.ringer_mode = mode, .interruption_filter = filter, .ring_volume = volume};
}

TEST(CallRingerOutputTest, TheRingFollowsThePhonesRinger) {
  struct Case {
    std::string name;
    std::optional<DeviceRingerReading> reading;
    bool sound;
    bool vibrate;
  };
  const std::vector<Case> cases = {
      {"normal, audible", Reading(kNormal, kAll, 5), true, false},
      {"normal, volume 0", Reading(kNormal, kAll, 0), false, false},
      {"vibrate", Reading(kVibrate, kAll, 5), false, true},
      {"vibrate, volume 0", Reading(kVibrate, kAll, 0), false, true},
      {"silent", Reading(kSilent, kAll, 5), false, false},
      {"DND priority, normal", Reading(kNormal, kPriority, 5), false, false},
      {"DND priority, vibrate", Reading(kVibrate, kPriority, 5), false, false},
      {"DND priority, silent", Reading(kSilent, kPriority, 5), false, false},
      {"DND none, normal", Reading(kNormal, kNone, 5), false, false},
      {"DND alarms, vibrate", Reading(kVibrate, kAlarms, 5), false, false},
      {"filter unknown, normal", Reading(kNormal, kUnknownFilter, 5), false, false},
      {"mode unknown", Reading(7, kAll, 5), false, false},
      {"could not read", std::nullopt, false, false},
  };
  for (const Case& c : cases) {
    const CallRingerOutput out = CallRingerOutputFor(c.reading);
    EXPECT_EQ(out.sound, c.sound) << c.name;
    EXPECT_EQ(out.vibrate, c.vibrate) << c.name;
  }
}

TEST(CallRingerOutputTest, ThePlatformsWithoutARingerRingAloud) {
  // DeviceRinger_Default's reading (desktop, iPhone): sound, no vibration.
  const CallRingerOutput out = CallRingerOutputFor(DeviceRingerReading{});
  EXPECT_TRUE(out.sound);
  EXPECT_FALSE(out.vibrate);
}

} // namespace
} // namespace pbr
