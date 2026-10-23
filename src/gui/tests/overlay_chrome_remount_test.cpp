#include "gui/shell/ShellFeedback.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/PinGateController.h"
#include "gui/shell/ShellPinGatePorts.h"
#include "foundation/crypto/PinDefaults.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

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

// Fresh install with a local backup: the fork carries the restore label; "Restore" tries the default
// PIN right away (no PIN page, never a new vault). When that is not the account's PIN the gate asks
// for it: one field (enter, not create), a wrong PIN stays on the page, cancel goes back.
TEST(PinGateRestoreTest, RestoreTriesDefaultPinThenAsksForTheAccountPin) {
  pbr::PinGateController controller;
  pbr::PinGateState seen;
  std::vector<std::pair<std::string, bool>> restores;
  int plain_pin_calls = 0;
  int cancels = 0;

  pbr::ShellPinGatePorts shell;
  shell.apply_pin_gate = [&](const pbr::PinGateState& state) { seen = state; };
  shell.pin_gate_snapshot = [&]() { return seen; };
  shell.remount_pin_gate = []() {};
  shell.dirty_pin_gate = []() {};
  controller.BindShellPinGate(std::move(shell));
  pbr::UnlockGateCompletePorts complete;
  complete.complete_restore_backup = [&](const std::string& pin, const bool set_default) {
    restores.emplace_back(pin, set_default);
  };
  complete.complete_with_pin = [&](const std::string&, bool) { ++plain_pin_calls; };
  complete.complete_with_default_pin = [&]() { ++plain_pin_calls; };
  complete.cancel = [&]() { ++cancels; };
  controller.BindGateComplete(std::move(complete));

  controller.ShowIdentityRestore("alice");
  EXPECT_TRUE(seen.identity_fork_mode);
  EXPECT_FALSE(seen.restore_label.empty()); // the translated label (tests have no catalog, so the key)

  controller.OnIdentityRestore();
  ASSERT_EQ(restores.size(), 1u);
  EXPECT_EQ(restores[0].first, pbr::kDefaultProfilePin);
  EXPECT_TRUE(restores[0].second);
  EXPECT_FALSE(seen.chooser_mode);
  EXPECT_FALSE(seen.create_mode);

  // The default PIN did not open it: the PIN page, enter mode.
  controller.ShowRestorePin(/*wrong_pin=*/false);
  EXPECT_TRUE(seen.active);
  EXPECT_TRUE(seen.restore_pin_mode);
  EXPECT_FALSE(seen.create_mode);
  EXPECT_FALSE(seen.chooser_mode);
  EXPECT_FALSE(seen.identity_fork_mode);
  EXPECT_TRUE(seen.error.empty());

  seen.pin = "0000";
  controller.OnSubmit();
  ASSERT_EQ(restores.size(), 2u);
  EXPECT_EQ(restores[1].first, "0000");
  EXPECT_FALSE(restores[1].second);
  EXPECT_EQ(plain_pin_calls, 0);

  controller.ShowRestorePin(/*wrong_pin=*/true);
  EXPECT_TRUE(seen.active);
  EXPECT_FALSE(seen.create_mode);
  EXPECT_FALSE(seen.error.empty());
  EXPECT_TRUE(seen.pin.empty());

  controller.OnCancel();
  EXPECT_EQ(cancels, 1);

  controller.BindGateComplete({});
  controller.BindShellPinGate({});
}
