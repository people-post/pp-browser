#include "gui/CallAudioFaultToastGate.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr int64_t kT0 = 1'000'000;
constexpr int64_t kHold = CallAudioFaultToastGate::kAudioFaultToastMs;

TEST(CallAudioFaultToastGateTest, BriefFaultNeverToasts) {
  CallAudioFaultToastGate gate;
  EXPECT_FALSE(gate.Update("call:a", true, kT0));
  EXPECT_FALSE(gate.Update("call:a", true, kT0 + 2'000));  // e.g. first audio not arrived yet
  EXPECT_FALSE(gate.Update("call:a", false, kT0 + 2'500));
  EXPECT_FALSE(gate.Update("call:a", true, kT0 + 3'000));  // clock restarted
  EXPECT_FALSE(gate.Update("call:a", true, kT0 + 3'000 + kHold - 1));
}

TEST(CallAudioFaultToastGateTest, LastingFaultToastsOncePerCall) {
  CallAudioFaultToastGate gate;
  EXPECT_FALSE(gate.Update("call:a", true, kT0));
  EXPECT_TRUE(gate.Update("call:a", true, kT0 + kHold));
  EXPECT_FALSE(gate.Update("call:a", true, kT0 + kHold + 5'000));
  EXPECT_FALSE(gate.Update("call:a", false, kT0 + kHold + 6'000));
  EXPECT_FALSE(gate.Update("call:a", true, kT0 + kHold + 7'000));
  EXPECT_FALSE(gate.Update("call:a", true, kT0 + 3 * kHold));  // same call: never again
}

TEST(CallAudioFaultToastGateTest, NewCallRestartsTheClock) {
  CallAudioFaultToastGate gate;
  EXPECT_FALSE(gate.Update("call:a", true, kT0));
  // Switched calls without a reset: the old fault start must not carry over.
  EXPECT_FALSE(gate.Update("call:b", true, kT0 + kHold));
  EXPECT_TRUE(gate.Update("call:b", true, kT0 + 2 * kHold));
}

TEST(CallAudioFaultToastGateTest, EachCallMayToastOnce) {
  CallAudioFaultToastGate gate;
  gate.Update("call:a", true, kT0);
  EXPECT_TRUE(gate.Update("call:a", true, kT0 + kHold));
  gate.Update("call:b", true, kT0 + kHold + 1);
  EXPECT_TRUE(gate.Update("call:b", true, kT0 + 2 * kHold + 1));
}

TEST(CallAudioFaultToastGateTest, ResetClearsThePendingFault) {
  CallAudioFaultToastGate gate;
  gate.Update("call:a", true, kT0);
  gate.Reset();
  EXPECT_FALSE(gate.Update("call:a", true, kT0 + kHold));  // clock restarted at this update
  EXPECT_TRUE(gate.Update("call:a", true, kT0 + 2 * kHold));
}

}  // namespace
}  // namespace pbr
