#include "feature/calls/LiveCallMedia.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// A coordinator belongs to one admission of a call: a re-admitted id starts with fresh media.
TEST(LiveCallMediaTest, OneCoordinatorPerAdmission) {
  CallMediaEngine engine;
  engine.SetSkipDeviceOpenForTest(true);
  LiveCalls live;
  LiveCallMedia media(live);

  live.AdmitPlaced("call:a", {"account:peer"});
  EXPECT_EQ(media.Media("call:a"), nullptr) << "no engine bound yet";
  media.BindMediaResources(&engine, nullptr);
  CallMediaCoordinator* first = media.Media("call:a");
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(media.Media("call:a"), first) << "the same admission keeps it";
  EXPECT_EQ(media.Media("call:unknown"), nullptr) << "never admitted here";

  EXPECT_EQ(first->DecideOnLocalAccept(1, std::nullopt), CallMediaPath::Direct);
  live.Close("call:a", LiveCallEndReason::LocalLeave);
  EXPECT_EQ(media.Media("call:a"), first) << "an ended call keeps it for its stops";

  live.AdmitPlaced("call:a", {"account:peer"});
  CallMediaCoordinator* second = media.Media("call:a");
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->MediaPath(), CallMediaPath::Undecided) << "a re-admitted id starts fresh";
}

} // namespace
} // namespace pbr
