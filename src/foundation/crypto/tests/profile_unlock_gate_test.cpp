#include "foundation/crypto/ProfileUnlockGate.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/PinDefaults.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/error/AppError.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

class ProfileUnlockGateTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_unlock_gate_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    auto initialized = engine_.Initialize(dir_.string());
    ASSERT_TRUE(static_cast<bool>(initialized)) << initialized.error().message;
    gate_.BindSecrets(engine_);
  }
  void TearDown() override {
    gate_.BindPorts({});
    engine_.Shutdown();
    std::filesystem::remove_all(dir_);
  }

  std::filesystem::path dir_;
  ProfileSecretsEngine engine_;
  ProfileUnlockGate gate_;
};

// Fresh install (no vault): the first frame is followed by the "I'm new / I already have an
// account" page, so the user is led into PIN creation and the first-run wizard instead of an
// empty home screen that only asks when some feature needs the vault (Kenneth, 2026-10-13).
TEST_F(ProfileUnlockGateTest, FreshInstallShowsIdentityForkAfterFirstPresent) {
  ASSERT_FALSE(engine_.HasVault());
  bool fork_shown = false;
  bool unlock_shown = false;
  ProfileUnlockPorts ports;
  ports.messaging_initialized = []() { return true; };
  ports.messaging_ready = []() { return false; };
  ports.ui.show_identity_fork = [&]() { fork_shown = true; };
  ports.ui.show_unlock = [&]() { unlock_shown = true; };
  gate_.BindPorts(std::move(ports));

  gate_.BeginDeferredUnlockAfterFirstPresent();

  EXPECT_TRUE(fork_shown);
  EXPECT_FALSE(unlock_shown);
  EXPECT_FALSE(gate_.IsUnlockInProgress());
}

// Without an account the app is unusable, so "back" on the PIN chooser / paste page returns to
// the identity fork instead of dismissing the gate onto an empty home screen.
TEST_F(ProfileUnlockGateTest, CancelWithoutVaultReturnsToIdentityFork) {
  ASSERT_FALSE(engine_.HasVault());
  int fork_shown = 0;
  bool dismissed = false;
  ProfileUnlockPorts ports;
  ports.messaging_initialized = []() { return true; };
  ports.messaging_ready = []() { return false; };
  ports.ui.show_identity_fork = [&]() { ++fork_shown; };
  ports.ui.dismiss = [&]() { dismissed = true; };
  gate_.BindPorts(std::move(ports));
  gate_.BeginDeferredUnlockAfterFirstPresent();
  ASSERT_EQ(fork_shown, 1);

  gate_.Cancel(); // "back" from the chooser / paste page

  EXPECT_EQ(fork_shown, 2);
  EXPECT_FALSE(dismissed);
}

// A local backup (uninstall-surviving store) turns the fresh-install page into "Restore my account:
// <nickname>" + the usual two buttons; without one, or when the store cannot be read, the plain fork.
TEST_F(ProfileUnlockGateTest, FreshInstallOffersRestoreWhenALocalBackupExists) {
  ASSERT_FALSE(engine_.HasVault());
  std::string restore_nickname;
  bool fork_shown = false;
  ProfileUnlockPorts ports;
  ports.messaging_initialized = []() { return true; };
  ports.messaging_ready = []() { return false; };
  ports.local_backup_nickname = []() -> Roe<std::optional<std::string>> { return std::optional<std::string>("alice"); };
  ports.ui.show_identity_restore = [&](const std::string& nickname) { restore_nickname = nickname; };
  ports.ui.show_identity_fork = [&]() { fork_shown = true; };
  gate_.BindPorts(std::move(ports));

  gate_.BeginDeferredUnlockAfterFirstPresent();

  EXPECT_EQ(restore_nickname, "alice");
  EXPECT_FALSE(fork_shown);
}

TEST_F(ProfileUnlockGateTest, FreshInstallFallsBackToTheForkWithoutABackup) {
  ASSERT_FALSE(engine_.HasVault());
  int restore_shown = 0;
  int fork_shown = 0;
  ProfileUnlockPorts ports;
  ports.messaging_initialized = []() { return true; };
  ports.messaging_ready = []() { return false; };
  ports.local_backup_nickname = []() -> Roe<std::optional<std::string>> { return std::optional<std::string>{}; };
  ports.ui.show_identity_restore = [&](const std::string&) { ++restore_shown; };
  ports.ui.show_identity_fork = [&]() { ++fork_shown; };
  gate_.BindPorts(std::move(ports));

  gate_.BeginDeferredUnlockAfterFirstPresent();
  EXPECT_EQ(restore_shown, 0);
  EXPECT_EQ(fork_shown, 1);

  gate_.Cancel(); // "back" from the PIN chooser lands on the same entry page
  EXPECT_EQ(fork_shown, 2);
}

// "Restore my account": the default PIN is tried first without asking; when it does not open the
// backup the PIN page comes up, a wrong PIN keeps it up, the right PIN restores and unlocks, and
// "back" returns to the restore entry.
TEST_F(ProfileUnlockGateTest, RestoreTriesTheDefaultPinThenAsksForTheAccountPin) {
  ASSERT_FALSE(engine_.HasVault());
  std::vector<std::string> tried;
  std::vector<bool> pin_page; // show_restore_pin(wrong_pin) calls
  int entry_shown = 0;
  bool dismissed = false;
  std::optional<bool> pin_is_default;
  ProfileUnlockPorts ports;
  ports.messaging_initialized = []() { return true; };
  ports.messaging_ready = []() { return false; };
  ports.local_backup_nickname = []() -> Roe<std::optional<std::string>> { return std::optional<std::string>("alice"); };
  ports.restore_local_backup = [&](const std::string& pin) -> Roe<void> {
    tried.push_back(pin);
    if (pin != "9876") {
      return AppError::Pin(Err::Pin::Generic, "Incorrect PIN or corrupt vault");
    }
    return engine_.Vault()->CreateWithDek(pin, ByteVector(kDataEncryptionKeySize, 0x61));
  };
  ports.set_pin_is_default = [&](const bool is_default) { pin_is_default = is_default; };
  ports.ui.show_identity_restore = [&](const std::string&) { ++entry_shown; };
  ports.ui.show_restore_pin = [&](const bool wrong_pin) { pin_page.push_back(wrong_pin); };
  ports.ui.dismiss = [&]() { dismissed = true; };
  gate_.BindPorts(std::move(ports));
  gate_.BeginDeferredUnlockAfterFirstPresent();
  ASSERT_EQ(entry_shown, 1);

  gate_.CompleteRestoreBackup(std::string(kDefaultProfilePin), /*set_default_pin=*/true);
  ASSERT_EQ(tried, std::vector<std::string>{std::string(kDefaultProfilePin)});
  ASSERT_EQ(pin_page, std::vector<bool>{false}); // asked for the PIN, no "wrong PIN" yet
  EXPECT_FALSE(gate_.IsUnlockInProgress());

  gate_.CompleteRestoreBackup("0000", /*set_default_pin=*/false);
  ASSERT_EQ(pin_page, (std::vector<bool>{false, true}));
  EXPECT_FALSE(dismissed);

  gate_.Cancel();
  EXPECT_EQ(entry_shown, 2);

  gate_.CompleteRestoreBackup("9876", /*set_default_pin=*/false);
  EXPECT_EQ(pin_page.size(), 2u);
  EXPECT_TRUE(dismissed);
  EXPECT_TRUE(engine_.IsUnlocked());
  ASSERT_TRUE(pin_is_default.has_value());
  EXPECT_FALSE(*pin_is_default);
}

} // namespace
} // namespace pbr
