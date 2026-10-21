#include "foundation/crypto/ProfileUnlockGate.h"

#include "foundation/crypto/ProfileSecretsEngine.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

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

} // namespace
} // namespace pbr
