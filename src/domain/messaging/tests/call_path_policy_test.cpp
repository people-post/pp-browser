#include "domain/messaging/CallMobility.h"
#include "domain/messaging/CallPathPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Clock = MobilityClassifier::Clock;
using std::chrono::minutes;
using std::chrono::seconds;

MobilityAttachment Wifi() { return {true, false, false}; }
MobilityAttachment Cellular() { return {true, true, true}; }
MobilityAttachment Hotspot() { return {true, false, true}; }

TEST(MobilityClassifierTest, UnknownUntilTheFirstAttachment) {
  MobilityClassifier c;
  EXPECT_EQ(c.Evaluate(Clock::now()), MobilityClass::Unknown);
}

TEST(MobilityClassifierTest, PrimarySignalsClassifyAtOnce) {
  const auto t0 = Clock::now();
  MobilityClassifier wifi;
  wifi.OnAttachment(Wifi(), false, t0);
  EXPECT_EQ(wifi.Evaluate(t0), MobilityClass::Stationary);
  MobilityClassifier cell;
  cell.OnAttachment(Cellular(), false, t0);
  EXPECT_EQ(cell.Evaluate(t0), MobilityClass::Mobile);
  MobilityClassifier hotspot;
  hotspot.OnAttachment(Hotspot(), false, t0);
  EXPECT_EQ(hotspot.Evaluate(t0), MobilityClass::Mobile) << "tethering looks like Wi-Fi but is metered";
}

// A laptop roaming between access points: three attachment changes in ten minutes → Mobile.
TEST(MobilityClassifierTest, ChurnMakesAPlainAttachmentMobile) {
  const auto t0 = Clock::now();
  MobilityClassifier c;
  c.OnAttachment(Wifi(), false, t0);
  c.OnAttachment(Wifi(), true, t0 + minutes(1));
  c.OnAttachment(Wifi(), true, t0 + minutes(3));
  EXPECT_EQ(c.Evaluate(t0 + minutes(3)), MobilityClass::Stationary) << "two changes: not yet";
  c.OnObservedAddressChanged(t0 + minutes(4));  // a NAT rebind counts too
  EXPECT_EQ(c.Evaluate(t0 + minutes(4)), MobilityClass::Mobile);
}

// Hysteresis: off cellular onto Wi-Fi, a phone stays Mobile until the attachment has been calm.
TEST(MobilityClassifierTest, BackToStationaryOnlyAfterCalm) {
  const auto t0 = Clock::now();
  MobilityClassifier c;
  c.OnAttachment(Cellular(), false, t0);
  ASSERT_EQ(c.Evaluate(t0), MobilityClass::Mobile);
  c.OnAttachment(Wifi(), true, t0 + minutes(1));
  EXPECT_EQ(c.Evaluate(t0 + minutes(1)), MobilityClass::Mobile) << "just moved";
  EXPECT_EQ(c.Evaluate(t0 + minutes(5)), MobilityClass::Mobile) << "4 min calm < 5";
  EXPECT_EQ(c.Evaluate(t0 + minutes(6) + seconds(1)), MobilityClass::Stationary);
}

TEST(MobilityClassifierTest, ChurnLeavesTheWindow) {
  const auto t0 = Clock::now();
  MobilityClassifier c;
  c.OnAttachment(Wifi(), false, t0);
  for (int i = 1; i <= 3; ++i) {
    c.OnAttachment(Wifi(), true, t0 + minutes(i));
  }
  ASSERT_EQ(c.Evaluate(t0 + minutes(3)), MobilityClass::Mobile);
  EXPECT_EQ(c.Evaluate(t0 + minutes(9)), MobilityClass::Mobile) << "still three in the window";
  EXPECT_EQ(c.Evaluate(t0 + minutes(14)), MobilityClass::Stationary) << "window drained, calm";
}

// A network change and the new observed address its re-probe reports are one event.
TEST(MobilityClassifierTest, ChurnSignalsCloseTogetherCountOnce) {
  const auto t0 = Clock::now();
  MobilityClassifier c;
  c.OnAttachment(Wifi(), false, t0);
  for (int i = 1; i <= 3; ++i) {
    c.OnAttachment(Wifi(), true, t0 + minutes(i));
    c.OnObservedAddressChanged(t0 + minutes(i) + seconds(3));
  }
  EXPECT_EQ(c.Evaluate(t0 + minutes(3)), MobilityClass::Mobile) << "three moves";
  MobilityClassifier d;
  d.OnAttachment(Wifi(), false, t0);
  d.OnAttachment(Wifi(), true, t0 + minutes(1));
  d.OnObservedAddressChanged(t0 + minutes(1) + seconds(3));
  d.OnAttachment(Wifi(), true, t0 + minutes(2));
  d.OnObservedAddressChanged(t0 + minutes(2) + seconds(3));
  EXPECT_EQ(d.Evaluate(t0 + minutes(2)), MobilityClass::Stationary) << "two moves, four signals";
}

TEST(MobilityClassifierTest, OfflineKeepsTheClass) {
  const auto t0 = Clock::now();
  MobilityClassifier c;
  c.OnAttachment(Cellular(), false, t0);
  ASSERT_EQ(c.Evaluate(t0), MobilityClass::Mobile);
  c.OnAttachment(MobilityAttachment{}, true, t0 + seconds(10));
  EXPECT_EQ(c.Evaluate(t0 + minutes(30)), MobilityClass::Mobile);
}

TEST(MobilityClassifierTest, OverridePins) {
  const auto t0 = Clock::now();
  MobilityClassifier c;
  c.OnAttachment(Cellular(), false, t0);
  c.SetOverride(MobilityClass::Stationary);
  EXPECT_EQ(c.Evaluate(t0), MobilityClass::Stationary);
  c.SetOverride(std::nullopt);
  EXPECT_EQ(c.Evaluate(t0), MobilityClass::Mobile);
}

TEST(MobilityWireTest, RoundTripsAndUnknownValues) {
  for (auto m : {MobilityClass::Unknown, MobilityClass::Stationary, MobilityClass::Mobile}) {
    EXPECT_EQ(ParseMobilityClass(MobilityClassWire(m)), m);
  }
  EXPECT_EQ(ParseMobilityClass(""), MobilityClass::Unknown);
  EXPECT_EQ(ParseMobilityClass("teleporting"), MobilityClass::Unknown) << "a newer value";
}

// Both ends compute the same policy: it is symmetric in the pair.
TEST(CallPathPolicyTest, BothEndsAgree) {
  const MobilityClass all[] = {MobilityClass::Unknown, MobilityClass::Stationary, MobilityClass::Mobile};
  for (auto a : all) {
    for (auto b : all) {
      const auto ab = DecideCallPathPolicy(a, b);
      const auto ba = DecideCallPathPolicy(b, a);
      EXPECT_EQ(ab.punch_at_start, ba.punch_at_start);
      EXPECT_EQ(ab.upgrade_to_direct, ba.upgrade_to_direct);
      EXPECT_EQ(ab.relay_role, ba.relay_role);
      EXPECT_EQ(ab.standby_priority, ba.standby_priority);
      EXPECT_EQ(ab.want_relay_standby, ba.want_relay_standby);
    }
  }
}

TEST(CallPathPolicyTest, PairTable) {
  const auto ss = DecideCallPathPolicy(MobilityClass::Stationary, MobilityClass::Stationary);
  EXPECT_TRUE(ss.punch_at_start);
  EXPECT_TRUE(ss.upgrade_to_direct);
  EXPECT_EQ(ss.relay_role, CallRelayRole::Standby);
  EXPECT_EQ(StandbyPriorityFor(ss, false), CallStandbyPriority::Low);
  EXPECT_EQ(StandbyPriorityFor(ss, true), CallStandbyPriority::Medium) << "a punched primary is fragile";

  const auto ms = DecideCallPathPolicy(MobilityClass::Mobile, MobilityClass::Stationary);
  EXPECT_FALSE(ms.punch_at_start);
  EXPECT_FALSE(ms.upgrade_to_direct);
  EXPECT_EQ(ms.relay_role, CallRelayRole::Anchor);
  EXPECT_EQ(StandbyPriorityFor(ms, false), CallStandbyPriority::High);

  // Unknown: Stationary for punching, Mobile for relay priority (K004).
  const auto us = DecideCallPathPolicy(MobilityClass::Unknown, MobilityClass::Stationary);
  EXPECT_TRUE(us.punch_at_start);
  EXPECT_TRUE(us.upgrade_to_direct);
  EXPECT_EQ(us.relay_role, CallRelayRole::Standby);
  EXPECT_EQ(StandbyPriorityFor(us, false), CallStandbyPriority::High);
  EXPECT_TRUE(us.want_relay_standby);
}

} // namespace
} // namespace pbr
