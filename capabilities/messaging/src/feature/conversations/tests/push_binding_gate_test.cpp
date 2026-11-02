#include "feature/conversations/PushBindingGate.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Action = PushBindingGate::Action;

TEST(PushBindingGateTest, SwitchAwaySuspendsAndUnbinds) {
  PushBindingGate gate;
  EXPECT_FALSE(gate.Suspended());
  EXPECT_EQ(gate.OnSwitchPending(true), Action::Unbind);
  EXPECT_TRUE(gate.Suspended());
}

TEST(PushBindingGateTest, UnbindSuccessIsNotRepeated) {
  PushBindingGate gate;
  ASSERT_EQ(gate.OnSwitchPending(true), Action::Unbind);
  gate.OnUnbound(true);
  EXPECT_EQ(gate.OnSwitchPending(true), Action::None);
}

TEST(PushBindingGateTest, UnbindFailureIsRetriedOnNextSave) {
  PushBindingGate gate;
  ASSERT_EQ(gate.OnSwitchPending(true), Action::Unbind);
  gate.OnUnbound(false);
  EXPECT_TRUE(gate.Suspended());
  EXPECT_EQ(gate.OnSwitchPending(true), Action::Unbind);
}

TEST(PushBindingGateTest, SwitchBackAfterUnbindRebinds) {
  PushBindingGate gate;
  ASSERT_EQ(gate.OnSwitchPending(true), Action::Unbind);
  gate.OnUnbound(true);
  EXPECT_EQ(gate.OnSwitchPending(false), Action::Rebind);
  EXPECT_FALSE(gate.Suspended());
}

TEST(PushBindingGateTest, SwitchBackAfterFailedUnbindOnlyResumes) {
  PushBindingGate gate;
  ASSERT_EQ(gate.OnSwitchPending(true), Action::Unbind);
  gate.OnUnbound(false);
  EXPECT_EQ(gate.OnSwitchPending(false), Action::None);
  EXPECT_FALSE(gate.Suspended());
}

TEST(PushBindingGateTest, SwitchBackDuringUnbindRebindsWhenItFinishes) {
  PushBindingGate gate;
  ASSERT_EQ(gate.OnSwitchPending(true), Action::Unbind);
  EXPECT_EQ(gate.OnSwitchPending(false), Action::None);
  EXPECT_TRUE(gate.OnUnbound(true));
  EXPECT_EQ(gate.OnSwitchPending(true), Action::Unbind);
}

TEST(PushBindingGateTest, UnbindFinishingWhileSwitchedAwayNeedsNoRebind) {
  PushBindingGate gate;
  ASSERT_EQ(gate.OnSwitchPending(true), Action::Unbind);
  EXPECT_FALSE(gate.OnUnbound(true));
}

TEST(PushBindingGateTest, NoPendingSwitchChangesNothing) {
  PushBindingGate gate;
  EXPECT_EQ(gate.OnSwitchPending(false), Action::None);
  EXPECT_FALSE(gate.Suspended());
}

} // namespace
} // namespace pbr
