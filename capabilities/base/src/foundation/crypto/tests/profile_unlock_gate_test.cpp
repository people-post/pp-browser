#include "foundation/crypto/ProfileUnlockGate.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/PinDefaults.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/error/AppError.h"

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <utility>
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
  ports.restore_recovery_phrase = [&](const SecretText& pin, const SecretText& phrase,
                                      const std::vector<std::string>&) -> Roe<void> {
    restores.emplace_back(pin.Reveal(), phrase.Reveal());
    if (phrase.Reveal() != "the right words") {
      return Error("No account was found for this recovery phrase");
    }
    return engine_.Vault()->CreateWithDek(pin.Reveal(), ByteVector(kDataEncryptionKeySize, 0x62));
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


// --- PIN grace window (#21 phase 1, DEV/PLAN-pin-grace.md) ---

class ProfileUnlockGateGraceTest : public ProfileUnlockGateTest {
protected:
  void SetUp() override {
    ProfileUnlockGateTest::SetUp();
    ASSERT_TRUE(static_cast<bool>(engine_.Vault()->CreateWithDek("4321", ByteVector(kDataEncryptionKeySize, 0x47))));
    engine_.Vault()->Lock();  // as after a cold start: the PIN is needed
    ASSERT_TRUE(engine_.NeedsUnlock());
  }

  /** Each try_grace_unlock answers the next status here (the last one repeats). */
  ProfileUnlockPorts Ports() {
    ProfileUnlockPorts ports;
    ports.messaging_initialized = []() { return true; };
    ports.messaging_ready = []() { return false; };
    ports.pin_is_default = [this]() { return pin_is_default_; };
    ports.set_pin_is_default = [this](const bool is_default) { pin_is_default_ = is_default; };
    ports.grace_minutes = []() { return 15; };
    ports.try_grace_unlock = [this]() -> Roe<GraceUnlock::Result> {
      events_.push_back("try");
      ++tries_;
      GraceUnlock::Result result;
      result.status = answers_.front();
      if (answers_.size() > 1) {
        answers_.pop_front();
      }
      if (result.status == GraceUnlock::Status::Found) {
        result.dek = ByteVector(kDataEncryptionKeySize, 0x47);
      }
      return result;
    };
    ports.remember_grace = [this](const int minutes) -> Roe<void> {
      remembered_.push_back(minutes);
      if (remember_fails_) {
        return Error("keychain refused");
      }
      return {};
    };
    ports.has_pending_push_call = [this]() { return pending_call_; };
    ports.protected_data_available = [this]() { return protected_data_; };
    ports.on_protected_data_available = [this](std::function<void()> fn) -> uint64_t {
      events_.push_back("register");
      handler_ = std::move(fn);
      return ++handler_ids_;
    };
    ports.remove_protected_data_handler = [this](const uint64_t id) {
      removed_.push_back(id);  // handler_ itself stays: it may be the one running
    };
    ports.run_on_ui = [](std::function<void()> fn) { fn(); };
    ports.run_later = [this](const std::chrono::milliseconds delay, std::function<void()> fn) {
      timers_.emplace_back(delay, std::move(fn));
    };
    ports.ui.show_unlock = [this]() { ++doors_; };
    ports.ui.dismiss = [this]() { ++dismissed_; };
    ports.ui.show_error = [this](const std::string&) { ++errors_; };
    ports.ui.show_waiting_device_unlock = [this](const bool waiting) { waiting_.push_back(waiting); };
    return ports;
  }

  void Answers(std::initializer_list<GraceUnlock::Status> answers) { answers_.assign(answers); }

  std::deque<GraceUnlock::Status> answers_{GraceUnlock::Status::Missing};
  bool pin_is_default_ = false;
  bool remember_fails_ = false;
  bool pending_call_ = false;
  bool protected_data_ = true;
  int tries_ = 0;
  int doors_ = 0;
  int dismissed_ = 0;
  int errors_ = 0;
  uint64_t handler_ids_ = 0;
  std::vector<std::string> events_;
  std::vector<int> remembered_;
  std::vector<uint64_t> removed_;
  std::vector<bool> waiting_;
  std::function<void()> handler_;
  std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> timers_;
};

// An open window: no PIN, no unlock screen, and the window is not extended (only the foreground does that).
TEST_F(ProfileUnlockGateGraceTest, AnOpenWindowUnlocksWithoutTheDoor) {
  Answers({GraceUnlock::Status::Found});
  gate_.BindPorts(Ports());

  gate_.BeginDeferredUnlockAfterFirstPresent();

  EXPECT_EQ(tries_, 1);
  EXPECT_EQ(doors_, 0);
  EXPECT_EQ(dismissed_, 1);
  EXPECT_FALSE(gate_.IsUnlockInProgress());
  EXPECT_FALSE(engine_.IsUnlocked()) << "no PIN (nor the dev PIN) was used: the fake grace entry opens nothing";
  EXPECT_TRUE(remembered_.empty()) << "a grace unlock never writes a new window";
}

// A push wake (the app started in the background, no foreground event) unlocking through the window never extends it:
// the gate has no touch, and a grace unlock is not followed by a remember either.
TEST_F(ProfileUnlockGateGraceTest, APushWakeThroughTheWindowNeverExtendsIt) {
  Answers({GraceUnlock::Status::Found});
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  gate_.OnForeground();  // nothing pending: no second read either

  EXPECT_EQ(tries_, 1);
  EXPECT_TRUE(remembered_.empty());
}

TEST_F(ProfileUnlockGateGraceTest, AnExpiredWindowAsksForThePin) {
  Answers({GraceUnlock::Status::Expired});
  gate_.BindPorts(Ports());

  gate_.BeginDeferredUnlockAfterFirstPresent();

  EXPECT_EQ(tries_, 1);
  EXPECT_EQ(doors_, 1) << "no dev PIN in this build: the unlock screen";
  EXPECT_EQ(dismissed_, 0);
  EXPECT_TRUE(waiting_.empty());
  EXPECT_EQ(handler_ids_, 0u) << "nothing to wait for";
}

TEST_F(ProfileUnlockGateGraceTest, AMissingEntryAsksForThePin) {
  Answers({GraceUnlock::Status::Missing});
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  EXPECT_EQ(doors_, 1);
  EXPECT_EQ(handler_ids_, 0u);
}

TEST_F(ProfileUnlockGateGraceTest, TheDefaultPinNeverReadsTheWindowNorKeepsOne) {
  pin_is_default_ = true;
  ASSERT_TRUE(static_cast<bool>(engine_.Vault()->ChangePin("4321", std::string(kDefaultProfilePin))));
  Answers({GraceUnlock::Status::Found});
  gate_.BindPorts(Ports());

  gate_.BeginDeferredUnlockAfterFirstPresent();

  EXPECT_EQ(tries_, 0);
  EXPECT_TRUE(engine_.IsUnlocked()) << "the silent default-PIN unlock, as before";
  EXPECT_TRUE(remembered_.empty());
}

TEST_F(ProfileUnlockGateGraceTest, WindowOffSkipsTheRead) {
  Answers({GraceUnlock::Status::Found});
  ProfileUnlockPorts ports = Ports();
  ports.grace_minutes = []() { return 0; };
  gate_.BindPorts(std::move(ports));
  gate_.BeginDeferredUnlockAfterFirstPresent();
  EXPECT_EQ(tries_, 0);
  EXPECT_EQ(doors_, 1);
}

TEST_F(ProfileUnlockGateGraceTest, APinUnlockKeepsTheWindow) {
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  ASSERT_EQ(doors_, 1);

  gate_.CompleteWithPin("4321", /*create_mode=*/false);

  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(remembered_, std::vector<int>{15});
}

// DEV/PLAN-pin-gate-redesign.md B: the app keeps the PIN's kind from the PIN that opened the vault — reported once per
// PIN unlock, with the PIN typed; a wrong PIN and the silent default-PIN unlock report nothing.
TEST_F(ProfileUnlockGateGraceTest, PinUnlockReportsVerifiedPinOnce) {
  std::vector<std::string> verified;
  ProfileUnlockPorts ports = Ports();
  ports.on_pin_verified = [&verified](const SecretText& pin) { verified.push_back(pin.Reveal()); };
  gate_.BindPorts(std::move(ports));
  gate_.BeginDeferredUnlockAfterFirstPresent();

  gate_.CompleteWithPin("0000", /*create_mode=*/false);
  EXPECT_TRUE(verified.empty()) << "a wrong PIN";

  gate_.CompleteWithPin("4321", /*create_mode=*/false);
  EXPECT_EQ(verified, std::vector<std::string>{"4321"});

  // The default PIN's silent unlock (a gate of its own: this one has started already).
  engine_.Vault()->Lock();
  ASSERT_TRUE(static_cast<bool>(engine_.Vault()->ChangePin("4321", std::string(kDefaultProfilePin))));
  pin_is_default_ = true;
  ProfileUnlockGate silent;
  silent.BindSecrets(engine_);
  ProfileUnlockPorts silent_ports = Ports();
  silent_ports.on_pin_verified = [&verified](const SecretText& pin) { verified.push_back(pin.Reveal()); };
  silent.BindPorts(std::move(silent_ports));
  silent.BeginDeferredUnlockAfterFirstPresent();
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(verified.size(), 1U) << "the default PIN is not a PIN the user chose";
  silent.BindPorts({});
}

TEST_F(ProfileUnlockGateGraceTest, AWrongPinKeepsNoWindow) {
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  gate_.CompleteWithPin("0000", /*create_mode=*/false);
  EXPECT_FALSE(engine_.IsUnlocked());
  EXPECT_TRUE(remembered_.empty());
}

TEST_F(ProfileUnlockGateGraceTest, AWindowNotKeptStillUnlocks) {
  remember_fails_ = true;
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();

  gate_.CompleteWithPin("4321", /*create_mode=*/false);

  EXPECT_EQ(remembered_.size(), 1u);
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(dismissed_, 1);
  EXPECT_EQ(errors_, 0) << "only a diagnostics line";
}

// An account brought here (a link, or its recovery phrase) under an own PIN keeps the window like a PIN unlock.
TEST_F(ProfileUnlockGateGraceTest, ALinkedDeviceKeepsTheWindow) {
  std::filesystem::remove(DataKeyVault::VaultPathForProfile(dir_.string()));
  ASSERT_FALSE(engine_.HasVault());
  ProfileUnlockPorts ports = Ports();
  ports.import_link_device = [this](const SecretText& pin, const std::string&) -> Roe<void> {
    return engine_.Vault()->CreateWithDek(pin.Reveal(), ByteVector(kDataEncryptionKeySize, 0x47));
  };
  gate_.BindPorts(std::move(ports));

  gate_.CompleteLinkDevice("4321", "{}", /*set_default_pin=*/false);

  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(remembered_, std::vector<int>{15});
}

TEST_F(ProfileUnlockGateGraceTest, ARecoveryRestoreKeepsTheWindow) {
  std::filesystem::remove(DataKeyVault::VaultPathForProfile(dir_.string()));
  ASSERT_FALSE(engine_.HasVault());
  ProfileUnlockPorts ports = Ports();
  ports.restore_recovery_phrase = [this](const SecretText& pin, const SecretText&,
                                         const std::vector<std::string>&) -> Roe<void> {
    return engine_.Vault()->CreateWithDek(pin.Reveal(), ByteVector(kDataEncryptionKeySize, 0x47));
  };
  gate_.BindPorts(std::move(ports));

  gate_.CompletePhraseRestore("4321", "the right words", /*set_default_pin=*/false);

  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(remembered_, std::vector<int>{15});
}

TEST_F(ProfileUnlockGateGraceTest, TheChooserDefaultPinKeepsNoWindow) {
  std::filesystem::remove(DataKeyVault::VaultPathForProfile(dir_.string()));
  ASSERT_FALSE(engine_.HasVault());
  gate_.BindPorts(Ports());
  gate_.CompleteWithDefaultPin();
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_TRUE(remembered_.empty());
}

// Locked phone, a call answered on its call screen: the handler is registered before the second read; that read
// finding the phone unlocked connects without the unlock screen.
TEST_F(ProfileUnlockGateGraceTest, ALockedPhoneUnlockedBetweenTheReadsConnectsWithoutTheDoor) {
  pending_call_ = true;
  Answers({GraceUnlock::Status::Unavailable, GraceUnlock::Status::Found});
  gate_.BindPorts(Ports());

  gate_.BeginDeferredUnlockAfterFirstPresent();

  EXPECT_EQ(events_, (std::vector<std::string>{"try", "register", "try"}));
  EXPECT_EQ(doors_, 0);
  EXPECT_EQ(dismissed_, 1);
  EXPECT_TRUE(waiting_.empty());
  EXPECT_EQ(removed_, std::vector<uint64_t>{1}) << "the handler goes when the gate finishes";
}

TEST_F(ProfileUnlockGateGraceTest, ALockedPhoneWithACallWaitsForTheUnlockThenConnects) {
  pending_call_ = true;
  Answers({GraceUnlock::Status::Unavailable, GraceUnlock::Status::Unavailable, GraceUnlock::Status::Found});
  gate_.BindPorts(Ports());

  gate_.BeginDeferredUnlockAfterFirstPresent();

  EXPECT_EQ(doors_, 1);
  EXPECT_EQ(waiting_, std::vector<bool>{true});
  ASSERT_EQ(timers_.size(), 1u);
  EXPECT_EQ(timers_[0].first, std::chrono::milliseconds(std::chrono::minutes(3)));
  ASSERT_TRUE(handler_);

  handler_();  // the phone was unlocked

  EXPECT_EQ(tries_, 3);
  EXPECT_EQ(dismissed_, 1);
  EXPECT_EQ(waiting_, (std::vector<bool>{true, false}));
  EXPECT_EQ(removed_.size(), 1u);
  timers_[0].second();  // a late timeout changes nothing
  EXPECT_EQ(waiting_.size(), 2u);
}

TEST_F(ProfileUnlockGateGraceTest, AFailedRetryStopsWaitingAndLeavesThePinScreen) {
  pending_call_ = true;
  Answers({GraceUnlock::Status::Unavailable, GraceUnlock::Status::Unavailable, GraceUnlock::Status::Expired});
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  ASSERT_TRUE(handler_);

  handler_();

  EXPECT_EQ(waiting_, (std::vector<bool>{true, false}));
  EXPECT_EQ(dismissed_, 0);
  EXPECT_EQ(doors_, 1) << "the unlock screen stays: type the PIN";
  EXPECT_EQ(removed_.size(), 1u) << "an expired window is not waited for";
}

TEST_F(ProfileUnlockGateGraceTest, ThreeMinutesWithoutTheUnlockLeaveThePinScreen) {
  pending_call_ = true;
  Answers({GraceUnlock::Status::Unavailable});
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  ASSERT_EQ(timers_.size(), 1u);

  timers_[0].second();

  EXPECT_EQ(waiting_, (std::vector<bool>{true, false}));
  EXPECT_EQ(dismissed_, 0);
  EXPECT_EQ(doors_, 1);

  gate_.CompleteWithPin("4321", /*create_mode=*/false);
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(dismissed_, 1);
  EXPECT_EQ(removed_.size(), 1u);
}

TEST_F(ProfileUnlockGateGraceTest, EnterPinInsteadStopsWaiting) {
  pending_call_ = true;
  Answers({GraceUnlock::Status::Unavailable});
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();

  gate_.UseDevicePinInstead();

  EXPECT_EQ(waiting_, (std::vector<bool>{true, false}));
  EXPECT_EQ(doors_, 1);
  ASSERT_EQ(timers_.size(), 1u);
  timers_[0].second();
  EXPECT_EQ(waiting_.size(), 2u) << "the timeout of a wait already over does nothing";
}

// Locked phone without a call (a push wake, a background refresh): the plain unlock screen at once; the phone's unlock
// or the next foreground may still open it without a PIN.
TEST_F(ProfileUnlockGateGraceTest, ALockedPhoneWithoutACallShowsTheDoorThenOpensOnForeground) {
  Answers({GraceUnlock::Status::Unavailable, GraceUnlock::Status::Unavailable, GraceUnlock::Status::Found});
  gate_.BindPorts(Ports());

  gate_.BeginDeferredUnlockAfterFirstPresent();

  EXPECT_EQ(doors_, 1);
  EXPECT_TRUE(waiting_.empty());
  EXPECT_TRUE(timers_.empty());
  EXPECT_TRUE(handler_);
  EXPECT_TRUE(removed_.empty()) << "kept for the phone's unlock";

  protected_data_ = false;
  gate_.OnForeground();
  EXPECT_EQ(tries_, 2) << "still locked: not read";

  protected_data_ = true;
  gate_.OnForeground();
  EXPECT_EQ(tries_, 3);
  EXPECT_EQ(dismissed_, 1);
  EXPECT_EQ(removed_.size(), 1u);
  EXPECT_TRUE(waiting_.empty());
}

TEST_F(ProfileUnlockGateGraceTest, ALockedPhoneWithoutACallOpensWhenThePhoneIsUnlocked) {
  Answers({GraceUnlock::Status::Unavailable, GraceUnlock::Status::Unavailable, GraceUnlock::Status::Unavailable,
           GraceUnlock::Status::Found});
  gate_.BindPorts(Ports());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  ASSERT_TRUE(handler_);

  handler_();  // a spurious notice: still unreadable, the handler stays
  EXPECT_EQ(dismissed_, 0);
  EXPECT_TRUE(removed_.empty());

  handler_();
  EXPECT_EQ(dismissed_, 1);
  EXPECT_TRUE(waiting_.empty());
}

// --- Face / fingerprint unlock (at-rest A014, DEV/PLAN-pin-gate-redesign.md C) ---

// Heavy work waits in a queue until the test runs it (the real run_heavy answers later, on the UI thread), and each
// face / fingerprint prompt answers the next status here (the last one repeats).
class ProfileUnlockGateBiometricTest : public ProfileUnlockGateGraceTest {
protected:
  ProfileUnlockPorts BioPorts() {
    ProfileUnlockPorts ports = Ports();
    ports.run_heavy = [this](std::function<Roe<void>()> work, std::function<void(Roe<void>)> on_done) {
      jobs_.emplace_back(std::move(work), std::move(on_done));
    };
    ports.biometric_enabled = [this]() { return bio_enabled_; };
    ports.try_biometric_unlock = [this](const std::string& prompt) -> Roe<BiometricUnlock::Result> {
      events_.push_back("bio");
      prompts_.push_back(prompt);
      BiometricUnlock::Result result;
      result.status = bio_answers_.front();
      if (bio_answers_.size() > 1) {
        bio_answers_.pop_front();
      }
      if (result.status == BiometricUnlock::Status::Missing) {
        bio_enabled_ = false;  // the engine deleted both pieces
      }
      return result;
    };
    ports.biometric_prompt_text = []() { return std::string("Unlock PP"); };
    ports.app_shown = [this]() { return app_shown_; };
    ports.on_biometric_lost = [this]() { ++lost_; };
    ports.ui.show_biometric_reset = [this](const std::string& hint) { hints_.push_back(hint); };
    ports.ui.set_unlock_in_progress = [this](const bool in_progress) { busy_ui_.push_back(in_progress); };
    return ports;
  }

  /** Runs the queued job at `index` (and nothing else). */
  void RunJob(const size_t index) {
    ASSERT_LT(index, jobs_.size());
    auto job = std::move(jobs_[index]);
    jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(index));
    job.second(job.first());
  }
  /** Runs queued jobs (and those they queue) until none is left. */
  void RunAll() {
    while (!jobs_.empty()) {
      RunJob(0);
    }
  }
  void BioAnswers(std::initializer_list<BiometricUnlock::Status> answers) { bio_answers_.assign(answers); }
  int Prompts() const { return static_cast<int>(prompts_.size()); }

  std::deque<std::pair<std::function<Roe<void>()>, std::function<void(Roe<void>)>>> jobs_;
  std::deque<BiometricUnlock::Status> bio_answers_{BiometricUnlock::Status::Canceled};
  bool bio_enabled_ = true;
  bool app_shown_ = true;
  int lost_ = 0;
  std::vector<std::string> prompts_;
  std::vector<std::string> hints_;
  std::vector<bool> busy_ui_;
};

// Order: the grace window, then the face / fingerprint prompt over the raised door (the dev PIN would come after; this
// build has none). An open window never prompts; neither does the app's default PIN.
TEST_F(ProfileUnlockGateBiometricTest, BiometricComesAfterGraceAndBeforeTheDevPin) {
  Answers({GraceUnlock::Status::Expired});
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  EXPECT_EQ(events_, (std::vector<std::string>{"try", "bio"}));
  EXPECT_EQ(doors_, 1) << "the door is up under the prompt";
  EXPECT_EQ(prompts_, std::vector<std::string>{"Unlock PP"});

  ProfileUnlockGate open_window;
  open_window.BindSecrets(engine_);
  Answers({GraceUnlock::Status::Found});
  events_.clear();
  open_window.BindPorts(BioPorts());
  open_window.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  EXPECT_EQ(events_, std::vector<std::string>{"try"}) << "the window opened it: no prompt";
  open_window.BindPorts({});

  pin_is_default_ = true;
  ASSERT_TRUE(static_cast<bool>(engine_.Vault()->ChangePin("4321", std::string(kDefaultProfilePin))));
  ProfileUnlockGate default_pin;
  default_pin.BindSecrets(engine_);
  events_.clear();
  default_pin.BindPorts(BioPorts());
  default_pin.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  EXPECT_TRUE(events_.empty()) << "the default PIN opens silently";
  default_pin.BindPorts({});
}

TEST_F(ProfileUnlockGateBiometricTest, DoorRaisedInForegroundPromptsOnce) {
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  EXPECT_EQ(doors_, 1);
  EXPECT_EQ(Prompts(), 1);

  gate_.OnForeground();
  RunAll();
  EXPECT_EQ(Prompts(), 1) << "once per door";
}

TEST_F(ProfileUnlockGateBiometricTest, DoorRaisedInBackgroundPromptsOnForeground) {
  app_shown_ = false;  // a push wake: the door rises behind the lock screen
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  EXPECT_EQ(doors_, 1);
  EXPECT_EQ(Prompts(), 0) << "no prompt off screen, and none used up";

  app_shown_ = true;
  gate_.OnForeground();
  RunAll();
  EXPECT_EQ(Prompts(), 1);
  gate_.OnForeground();
  RunAll();
  EXPECT_EQ(Prompts(), 1);
}

TEST_F(ProfileUnlockGateBiometricTest, CanceledDoesNotRepromptOnForeground) {
  BioAnswers({BiometricUnlock::Status::Canceled});
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  ASSERT_EQ(Prompts(), 1);
  EXPECT_EQ(dismissed_, 0) << "stays on the door";
  EXPECT_EQ(errors_, 0) << "a cancel is no error";

  gate_.OnForeground();
  gate_.OnForeground();
  RunAll();
  EXPECT_EQ(Prompts(), 1);

  gate_.TryBiometric();  // the door's key: always again
  RunAll();
  EXPECT_EQ(Prompts(), 2);

  gate_.CompleteWithPin("4321", /*create_mode=*/false);
  RunAll();
  EXPECT_TRUE(engine_.IsUnlocked()) << "the PIN still opens it";
}

// The grace window's retry (a locked phone's entry read again) never prompts itself; once it no longer opens, the door
// needs the PIN and prompts as a door just raised.
TEST_F(ProfileUnlockGateBiometricTest, GraceRetryDoesNotPrompt) {
  Answers({GraceUnlock::Status::Unavailable, GraceUnlock::Status::Unavailable, GraceUnlock::Status::Unavailable,
           GraceUnlock::Status::Unavailable, GraceUnlock::Status::Expired});
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  EXPECT_EQ(doors_, 1);
  EXPECT_EQ(Prompts(), 0) << "the grace entry is still awaited";
  ASSERT_TRUE(handler_);

  handler_();  // the phone's unlock notice: still unreadable
  RunAll();
  EXPECT_EQ(Prompts(), 0);
  gate_.OnForeground();  // the foreground retries the grace entry, no prompt
  RunAll();
  EXPECT_EQ(tries_, 4);
  EXPECT_EQ(Prompts(), 0);

  gate_.OnForeground();  // the window has expired meanwhile: the PIN is needed now
  RunAll();
  EXPECT_EQ(tries_, 5);
  EXPECT_EQ(Prompts(), 1);
}

// PIN and face / fingerprint never run at once: a PIN submitted while the prompt runs is declined (the door is told it
// is not busy, so the pad takes keys again); once the prompt has answered, the PIN opens it.
TEST_F(ProfileUnlockGateBiometricTest, PinSubmitDuringBiometricIsDeclined) {
  BioAnswers({BiometricUnlock::Status::Canceled});
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunJob(0);  // the grace read: the door rises, the prompt is queued
  ASSERT_EQ(jobs_.size(), 1U);
  busy_ui_.clear();

  gate_.CompleteWithPin("4321", /*create_mode=*/false);
  EXPECT_EQ(jobs_.size(), 1U) << "no unlock started beside the prompt";
  EXPECT_EQ(busy_ui_, std::vector<bool>{false}) << "the door hears it was declined";
  EXPECT_FALSE(gate_.IsUnlockInProgress());

  RunAll();  // the prompt is canceled
  gate_.CompleteWithPin("4321", /*create_mode=*/false);
  RunAll();
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(dismissed_, 1);
  EXPECT_EQ(remembered_, std::vector<int>{15});
}

// A PIN submitted while a grace retry runs (a locked phone's entry read again) is declined the same way, not dropped
// silently with the pad left busy.
TEST_F(ProfileUnlockGateBiometricTest, PinSubmitDuringAGraceRetryIsDeclined) {
  bio_enabled_ = false;
  Answers({GraceUnlock::Status::Unavailable, GraceUnlock::Status::Unavailable, GraceUnlock::Status::Expired});
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  ASSERT_EQ(doors_, 1);
  ASSERT_TRUE(handler_);
  handler_();  // the phone's unlock: the retry is queued, an unlock in progress
  ASSERT_EQ(jobs_.size(), 1U);
  ASSERT_TRUE(gate_.IsUnlockInProgress());
  busy_ui_.clear();

  gate_.CompleteWithPin("4321", /*create_mode=*/false);
  EXPECT_EQ(jobs_.size(), 1U);
  EXPECT_EQ(busy_ui_, std::vector<bool>{false});

  RunAll();  // the window has expired: the door stays
  gate_.CompleteWithPin("4321", /*create_mode=*/false);
  RunAll();
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(dismissed_, 1);
}

TEST_F(ProfileUnlockGateBiometricTest, BiometricResultAfterGateClosedIsDropped) {
  BioAnswers({BiometricUnlock::Status::Found});
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunJob(0);
  ASSERT_EQ(jobs_.size(), 1U);

  gate_.Cancel();  // the gate finishes without an unlock
  ASSERT_EQ(dismissed_, 1);
  RunAll();
  EXPECT_EQ(dismissed_, 1);
  EXPECT_TRUE(remembered_.empty());
  EXPECT_FALSE(gate_.IsUnlockInProgress());
}

TEST_F(ProfileUnlockGateBiometricTest, BiometricKeyIgnoredWhileInFlight) {
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunJob(0);
  ASSERT_EQ(jobs_.size(), 1U) << "the prompt runs";

  gate_.TryBiometric();
  gate_.TryBiometric();
  EXPECT_EQ(jobs_.size(), 1U) << "no second prompt while one runs";
  RunAll();
  EXPECT_EQ(Prompts(), 1);

  gate_.TryBiometric();  // answered (canceled): the key prompts again
  EXPECT_EQ(jobs_.size(), 1U);
  RunAll();
  EXPECT_EQ(Prompts(), 2);
}

// Found: the door closes like after a PIN unlock, and the grace window starts.
TEST_F(ProfileUnlockGateBiometricTest, BiometricHitUnlocksAndKeepsTheWindow) {
  BioAnswers({BiometricUnlock::Status::Found});
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  EXPECT_EQ(Prompts(), 1);
  EXPECT_EQ(dismissed_, 1);
  EXPECT_EQ(remembered_, std::vector<int>{15});
  EXPECT_FALSE(gate_.IsUnlockInProgress());
}

// Missing (enrolment changed, the DEK replaced): the engine deleted both pieces; the door's key goes with a hint, the
// settings switch is told it is off, and nothing prompts again.
TEST_F(ProfileUnlockGateBiometricTest, MissingTurnsTheToggleOff) {
  BioAnswers({BiometricUnlock::Status::Missing});
  gate_.BindPorts(BioPorts());
  gate_.BeginDeferredUnlockAfterFirstPresent();
  RunAll();
  EXPECT_EQ(Prompts(), 1);
  EXPECT_EQ(lost_, 1);
  ASSERT_EQ(hints_.size(), 1U);
  EXPECT_FALSE(hints_[0].empty());
  EXPECT_EQ(dismissed_, 0);
  EXPECT_EQ(errors_, 0);

  gate_.TryBiometric();
  EXPECT_TRUE(jobs_.empty()) << "the switch is off now";
  gate_.CompleteWithPin("4321", /*create_mode=*/false);
  RunAll();
  EXPECT_TRUE(engine_.IsUnlocked());
}

} // namespace
} // namespace pbr
