#include "lib/calls/CallControlCodec.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

CallRosterDetail Roster(const bool on_hold, const bool video_paused) {
  CallRosterDetail roster;
  roster.call_id = "call:1";
  CallRosterEntry entry;
  entry.identity = "account:bob";
  entry.on_hold = on_hold;
  entry.video_paused = video_paused;
  roster.participants.push_back(entry);
  return roster;
}

// A member on hold, or with its camera paused by the background, says so in the roster.
TEST(CallRosterCodecTest, HoldAndPausedCameraRoundTrip) {
  const std::string json = CallControlCodec::EncodeRoster(Roster(true, true)).value();
  const CallRosterDetail back = CallControlCodec::DecodeRoster(json).value();
  ASSERT_EQ(back.participants.size(), 1u);
  EXPECT_TRUE(back.participants[0].on_hold);
  EXPECT_TRUE(back.participants[0].video_paused);
}

// Off, the fields stay off the wire; an older peer's roster (no fields) reads as neither.
TEST(CallRosterCodecTest, OffIsAbsentAndAbsentIsOff) {
  const std::string json = CallControlCodec::EncodeRoster(Roster(false, false)).value();
  EXPECT_EQ(json.find("on_hold"), std::string::npos);
  EXPECT_EQ(json.find("video_paused"), std::string::npos);
  const CallRosterDetail back = CallControlCodec::DecodeRoster(json).value();
  ASSERT_EQ(back.participants.size(), 1u);
  EXPECT_FALSE(back.participants[0].on_hold);
  EXPECT_FALSE(back.participants[0].video_paused);
}

} // namespace
} // namespace pbr
