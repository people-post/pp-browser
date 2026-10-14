#include "gui/shell/ShellFeedback.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/PinGateController.h"
#include "gui/shell/ShellPinGatePorts.h"

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

// Fresh install with a local backup: the fork carries the restore label; "Restore" goes to the PIN
// chooser, and either PIN choice completes the restore (never a plain new vault).
TEST(PinGateRestoreTest, RestoreLabelThenChooserThenCompleteRestore) {
  pbr::PinGateController controller;
  pbr::PinGateState seen;
  std::string restore_pin;
  int restore_calls = 0;
  bool default_pin = false;
  int plain_default_calls = 0;

  pbr::ShellPinGatePorts shell;
  shell.apply_pin_gate = [&](const pbr::PinGateState& state) { seen = state; };
  shell.pin_gate_snapshot = [&]() { return seen; };
  shell.remount_pin_gate = []() {};
  shell.dirty_pin_gate = []() {};
  controller.BindShellPinGate(std::move(shell));
  pbr::UnlockGateCompletePorts complete;
  complete.complete_restore_backup = [&](const std::string& pin, const bool set_default) {
    ++restore_calls;
    restore_pin = pin;
    default_pin = set_default;
  };
  complete.complete_with_default_pin = [&]() { ++plain_default_calls; };
  controller.BindGateComplete(std::move(complete));

  controller.ShowIdentityRestore("alice");
  EXPECT_TRUE(seen.identity_fork_mode);
  EXPECT_FALSE(seen.restore_label.empty()); // the translated label (tests have no catalog, so the key)

  controller.OnIdentityRestore();
  EXPECT_TRUE(seen.chooser_mode);
  EXPECT_FALSE(seen.identity_fork_mode);

  controller.OnUseDefaultPin();
  EXPECT_EQ(restore_calls, 1);
  EXPECT_TRUE(default_pin);
  EXPECT_EQ(plain_default_calls, 0);
  EXPECT_FALSE(restore_pin.empty());

  controller.BindGateComplete({});
  controller.BindShellPinGate({});
}
