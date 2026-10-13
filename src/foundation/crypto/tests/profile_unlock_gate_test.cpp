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

} // namespace
} // namespace pbr
