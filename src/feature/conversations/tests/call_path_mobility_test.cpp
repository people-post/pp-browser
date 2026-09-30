#include "feature/calls/CallPathMobility.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Clock = CallPathMobility::Clock;
using std::chrono::minutes;

MobilityAttachment Wifi() { return {true, false, false}; }

// k6, passive: events report a class flip; the owner wakes it at NextWakeAt and a churn-driven
// Mobile relaxes there with no new event.
TEST(CallPathMobilityTest, EventsReportFlipsAndTheWakeRelaxesChurn) {
  const auto t0 = Clock::now();
  CallPathMobility mobility;
  EXPECT_TRUE(mobility.OnAttachment(Wifi(), false, t0)) << "Unknown -> Stationary";
  EXPECT_EQ(mobility.LocalClass(), MobilityClass::Stationary);
  EXPECT_FALSE(mobility.NextWakeAt(t0)) << "nothing to relax";

  EXPECT_FALSE(mobility.OnAttachment(Wifi(), true, t0 + minutes(1)));
  EXPECT_FALSE(mobility.OnAttachment(Wifi(), true, t0 + minutes(2)));
  EXPECT_TRUE(mobility.OnAttachment(Wifi(), true, t0 + minutes(3))) << "third move: Mobile";
  EXPECT_EQ(mobility.LocalClass(), MobilityClass::Mobile);

  // The owner wakes it at each reported deadline, with no new event, until it settles.
  auto now = t0 + minutes(3);
  auto wake = mobility.NextWakeAt(now);
  ASSERT_TRUE(wake);
  EXPECT_FALSE(mobility.OnWake(*wake - minutes(1))) << "too early";
  bool flipped = false;
  for (int hops = 0; hops < 5 && wake; ++hops) {
    now = *wake;
    flipped = mobility.OnWake(now) || flipped;
    wake = mobility.NextWakeAt(now);
  }
  EXPECT_TRUE(flipped) << "the wake reported the flip back";
  EXPECT_EQ(mobility.LocalClass(), MobilityClass::Stationary);
  EXPECT_FALSE(mobility.NextWakeAt(now)) << "settled: nothing to wake for";
}

TEST(CallPathMobilityTest, APeersClassChangeIsReportedOnce) {
  CallPathMobility mobility;
  EXPECT_TRUE(mobility.NoteRemote("call:a", MobilityClass::Mobile));
  EXPECT_FALSE(mobility.NoteRemote("call:a", MobilityClass::Mobile)) << "unchanged";
  EXPECT_TRUE(mobility.NoteRemote("call:a", MobilityClass::Stationary));
  EXPECT_FALSE(mobility.NoteRemote("", MobilityClass::Mobile));
}

} // namespace
} // namespace pbr
