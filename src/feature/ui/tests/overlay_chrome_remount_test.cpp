#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellFeedbackPorts.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/ShellPinGatePorts.h"

#include <gtest/gtest.h>

TEST(DialogChromeRemountTest, ShowAlertRemountsDialogNotSyncLayout) {
  pbr::ShellState state;
  int remount_count = 0;
  int sync_count = 0;

  pbr::ShellFeedbackChromePorts ports;
  ports.shell_state = [&state]() -> pbr::ShellState& { return state; };
  ports.remount_dialog = [&]() { ++remount_count; };
  ports.dirty_feedback = []() {};
  pbr::ShellFeedback::BindChromePorts(std::move(ports));

  pbr::ShellFeedback::ShowAlert(state, "Title", "Message");
  EXPECT_TRUE(state.dialog.active);
  EXPECT_EQ(remount_count, 1);
  EXPECT_EQ(sync_count, 0);

  pbr::ShellFeedback::DialogOk(state);
  EXPECT_FALSE(state.dialog.active);
  EXPECT_EQ(remount_count, 2);

  pbr::ShellFeedback::BindChromePorts({});
}

TEST(PinGateChromeRemountTest, ShowAndDismissRemountPinGate) {
  pbr::PinGateController controller;
  int remount_count = 0;

  pbr::ShellPinGatePorts ports;
  ports.apply_pin_gate = [](const pbr::PinGateState&) {};
  ports.pin_gate_snapshot = []() { return pbr::PinGateState{}; };
  ports.remount_pin_gate = [&]() { ++remount_count; };
  ports.dirty_pin_gate = []() {};
  controller.BindShellPinGate(std::move(ports));

  controller.ShowUnlock();
  EXPECT_EQ(remount_count, 1);
  controller.Dismiss();
  EXPECT_EQ(remount_count, 2);

  controller.BindShellPinGate({});
}
