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
TEST(ProfileUnlockGateWrongPinTest, OnlyTheVaultRefusalIsAWrongPin) {
  EXPECT_TRUE(ProfileUnlockGate::IsWrongPinError(AppError::Pin(Err::Pin::Generic, "Incorrect PIN or corrupt vault")));
  EXPECT_FALSE(ProfileUnlockGate::IsWrongPinError(AppError::Pin(Err::Pin::VaultUnavailable, "no vault")));
  EXPECT_FALSE(ProfileUnlockGate::IsWrongPinError(Error("disk full")));
}

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

// "Restore with my recovery phrase" (multi-device M023): the phrase restored under the PIN chosen for this device,
// then unlocked; no first-run wizard (as after a link). A phrase that finds no account leaves the vault absent.
TEST_F(ProfileUnlockGateTest, APhraseRestoreUnlocksUnderThePinChosenHere) {
  ASSERT_FALSE(engine_.HasVault());
  std::vector<std::pair<std::string, std::string>> restores;  // pin, phrase
  int imported_notices = 0;
  int errors = 0;
  bool dismissed = false;
  std::optional<bool> pin_is_default;
  ProfileUnlockPorts ports;
  ports.messaging_initialized = []() { return true; };
  ports.messaging_ready = []() { return false; };
  ports.restore_recovery_phrase = [&](const std::string& pin, const std::string& phrase) -> Roe<void> {
    restores.emplace_back(pin, phrase);
    if (phrase != "the right words") {
      return Error("No account was found for this recovery phrase");
    }
    return engine_.Vault()->CreateWithDek(pin, ByteVector(kDataEncryptionKeySize, 0x62));
  };
  ports.set_pin_is_default = [&](const bool is_default) { pin_is_default = is_default; };
  ports.ui.on_link_imported = [&]() { ++imported_notices; };
  ports.ui.show_error = [&](const std::string&) { ++errors; };
  ports.ui.dismiss = [&]() { dismissed = true; };
  gate_.BindPorts(std::move(ports));

  gate_.CompletePhraseRestore("4321", "unknown words", /*set_default_pin=*/false);
  EXPECT_EQ(errors, 1);
  EXPECT_FALSE(engine_.HasVault());
  EXPECT_FALSE(gate_.IsUnlockInProgress()) << "the user can try again";

  gate_.CompletePhraseRestore("4321", "the right words", /*set_default_pin=*/false);
  ASSERT_EQ(restores.size(), 2u);
  EXPECT_EQ(restores.back(), std::make_pair(std::string("4321"), std::string("the right words")));
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_TRUE(dismissed);
  EXPECT_EQ(imported_notices, 1) << "no first-run wizard for an account that came back";
  ASSERT_TRUE(pin_is_default.has_value());
  EXPECT_FALSE(*pin_is_default);
}

} // namespace
} // namespace pbr
