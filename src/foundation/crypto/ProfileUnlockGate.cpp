#include "foundation/crypto/ProfileUnlockGate.h"

#include "foundation/crypto/PinDefaults.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"
#include "common/StartupTiming.h"

#include <memory>
#include <stdexcept>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

ProfileUnlockGate::ProfileUnlockGate() {
  redirectLogger("ProfileUnlockGate");
}

Roe<void> UnlockProfileSecretsAndReady(ProfileSecretsEngine& secrets, const std::string& pin,
                                       const std::function<Roe<void>()>& ensure_messaging_ready) {
  {
    StartupPhase phase("ProfileUnlockGate::VaultUnlock");
    if (auto unlocked = secrets.Unlock(pin); !unlocked) {
      return unlocked.error();
    }
  }
  if (!ensure_messaging_ready) {
    return {};
  }
  return ensure_messaging_ready();
}

void ProfileUnlockGate::BindSecrets(ProfileSecretsEngine& secrets) {
  secrets_ = &secrets;
}

void ProfileUnlockGate::BindPorts(ProfileUnlockPorts ports) {
  ports_ = std::move(ports);
}

ProfileSecretsEngine& ProfileUnlockGate::Secrets() {
  if (!secrets_) {
    throw std::runtime_error("ProfileUnlockGate secrets not bound");
  }
  return *secrets_;
}

Roe<void> ProfileUnlockGate::UnlockAndReady(const std::string& pin) {
  return UnlockProfileSecretsAndReady(Secrets(), pin, ports_.ensure_messaging_ready);
}

void ProfileUnlockGate::DrainQueue(const bool unlocked) {
  std::vector<std::function<void(bool)>> queued;
  queued.swap(pending_);
  showing_ = false;
  for (auto& cb : queued) {
    if (cb) {
      cb(unlocked);
    }
  }
}

void ProfileUnlockGate::SetUnlockInProgress(const bool in_progress) {
  unlock_in_progress_ = in_progress;
  if (ports_.ui.set_unlock_in_progress) {
    ports_.ui.set_unlock_in_progress(in_progress);
  }
}

void ProfileUnlockGate::Finish(const bool unlocked) {
  SetUnlockInProgress(false);
  if (ports_.ui.dismiss) {
    ports_.ui.dismiss();
  }
  DrainQueue(unlocked);
}

void ProfileUnlockGate::ReportError(const std::string& message) {
  log().warning << "Unlock failed: " << message;
  SetUnlockInProgress(false);
  if (!showing_) {
    // Silent unlock failed with no modal yet — open unlock so the user can retry.
    RequestShowUnlock(nullptr);
  }
  if (ports_.ui.show_error) {
    ports_.ui.show_error(message);
  }
}

void ProfileUnlockGate::ReportError(const Error& error) {
  // The vault's text ("Incorrect PIN or corrupt vault") is English; the log keeps it, the screen
  // gets our wording. A silent unlock (no PIN typed) opens the unlock screen without that line.
  if (!IsWrongPinError(error)) {
    ReportError(error.message);
    return;
  }
  log().warning << "Unlock failed: " << error.message;
  SetUnlockInProgress(false);
  const bool silent = !showing_;
  if (silent) {
    RequestShowUnlock(nullptr);
  }
  if (!silent && ports_.ui.show_error) {
    ports_.ui.show_error(Tr("pin.error.wrong"));
  }
}

bool ProfileUnlockGate::IsWrongPinError(const Error& error) {
  return AppError::CategoryOf(error) == ErrorCategory::Pin && error.code == static_cast<int32_t>(Err::Pin::Generic);
}

void ProfileUnlockGate::RequestShowIdentityFork(std::function<void(bool)> done) {
  if (done) {
    pending_.push_back(std::move(done));
  }
  if (showing_) {
    return;
  }
  showing_ = true;
  if (ports_.ui.show_identity_fork) {
    ports_.ui.show_identity_fork();
    return;
  }
  if (ports_.ui.show_chooser) {
    ports_.ui.show_chooser();
  }
}

void ProfileUnlockGate::RequestShowFreshInstallEntry() {
  std::optional<std::string> nickname;
  if (ports_.local_backup_nickname) {
    if (auto peeked = ports_.local_backup_nickname(); peeked) {
      nickname = *peeked;
    } else {
      log().warning << "Local account backup unreadable, offering a new account: " << peeked.error().message;
    }
  }
  if (nickname && ports_.ui.show_identity_restore) {
    if (showing_) {
      return;
    }
    showing_ = true;
    ports_.ui.show_identity_restore(*nickname);
    return;
  }
  RequestShowIdentityFork(nullptr);
}

void ProfileUnlockGate::RequestShowChooser(std::function<void(bool)> done) {
  if (done) {
    pending_.push_back(std::move(done));
  }
  if (showing_) {
    return;
  }
  showing_ = true;
  if (ports_.ui.show_chooser) {
    ports_.ui.show_chooser();
  }
}

void ProfileUnlockGate::RequestShowUnlock(std::function<void(bool)> done) {
  if (done) {
    pending_.push_back(std::move(done));
  }
  if (showing_) {
    return;
  }
  showing_ = true;
  if (ports_.ui.show_unlock) {
    ports_.ui.show_unlock();
  }
}

void ProfileUnlockGate::RunUnlockAndReadyAsync(std::string pin, const bool set_default_pin,
                                               const bool clear_default_pin) {
  if (unlock_in_progress_) {
    return;
  }
  SetUnlockInProgress(true);
  StartupMark("unlock_and_ready_begin");

  auto work = [this, pin = std::move(pin)]() -> Roe<void> { return UnlockAndReady(pin); };
  auto on_done = [this, set_default_pin, clear_default_pin](Roe<void> result) {
    if (!result) {
      StartupMark("unlock_and_ready_fail");
      ReportError(result.error());
      return;
    }
    if (set_default_pin && ports_.set_pin_is_default) {
      ports_.set_pin_is_default(true);
    }
    if (clear_default_pin && ports_.set_pin_is_default) {
      ports_.set_pin_is_default(false);
    }
    StartupMark("unlock_and_ready_ok");
    if (set_default_pin && ports_.ui.on_default_provisioned) {
      ports_.ui.on_default_provisioned();
    }
    Finish(true);
  };

  if (ports_.run_heavy) {
    ports_.run_heavy(std::move(work), std::move(on_done));
    return;
  }
  on_done(work());
}

void ProfileUnlockGate::EnsureUnlocked(std::function<void(bool unlocked)> done) {
  if (!ports_.messaging_initialized || !ports_.messaging_initialized()) {
    if (done) {
      done(false);
    }
    return;
  }
  if (ports_.messaging_ready && ports_.messaging_ready()) {
    if (done) {
      done(true);
    }
    return;
  }
  if (unlock_in_progress_) {
    if (done) {
      pending_.push_back(std::move(done));
    }
    return;
  }
  if (Secrets().IsUnlocked()) {
    if (!ports_.ensure_messaging_ready) {
      if (done) {
        done(false);
      }
      return;
    }
    if (done) {
      pending_.push_back(std::move(done));
    }
    // Vault already open — still need messaging stack (may block); keep UI responsive.
    SetUnlockInProgress(true);
    StartupMark("ensure_messaging_ready_begin");
    auto work = [this]() -> Roe<void> {
      if (!ports_.ensure_messaging_ready) {
        return Error("ensure_messaging_ready unavailable");
      }
      return ports_.ensure_messaging_ready();
    };
    auto on_done = [this](Roe<void> result) {
      StartupMark(result ? "ensure_messaging_ready_ok" : "ensure_messaging_ready_fail");
      Finish(static_cast<bool>(result));
    };
    if (ports_.run_heavy) {
      ports_.run_heavy(std::move(work), std::move(on_done));
    } else {
      on_done(work());
    }
    return;
  }
  if (!Secrets().HasVault()) {
    RequestShowIdentityFork(std::move(done));
    return;
  }
  if (ports_.pin_is_default && ports_.pin_is_default()) {
    if (done) {
      pending_.push_back(std::move(done));
    }
    BeginDeferredUnlockAfterFirstPresent();
    return;
  }
  RequestShowUnlock(std::move(done));
}

void ProfileUnlockGate::BeginDeferredUnlockAfterFirstPresent() {
  if (deferred_unlock_started_) {
    return;
  }
  deferred_unlock_started_ = true;

  if (!ports_.messaging_initialized || !ports_.messaging_initialized()) {
    DrainQueue(false);
    return;
  }
  if (ports_.messaging_ready && ports_.messaging_ready()) {
    DrainQueue(true);
    return;
  }
  if (!Secrets().NeedsUnlock()) {
    if (Secrets().IsUnlocked()) {
      if (!ports_.ensure_messaging_ready) {
        DrainQueue(false);
        return;
      }
      SetUnlockInProgress(true);
      StartupMark("deferred_ensure_messaging_begin");
      auto work = [this]() -> Roe<void> { return ports_.ensure_messaging_ready(); };
      auto on_done = [this](Roe<void> result) {
        StartupMark(result ? "deferred_ensure_messaging_ok" : "deferred_ensure_messaging_fail");
        Finish(static_cast<bool>(result));
      };
      if (ports_.run_heavy) {
        ports_.run_heavy(std::move(work), std::move(on_done));
      } else {
        on_done(work());
      }
    } else {
      // No vault yet (fresh install): go straight to "I'm new / I already have an account" (plus
      // "Restore my account" when a local backup exists), so PIN creation and the first-run wizard
      // follow the first frame instead of an empty home screen.
      RequestShowFreshInstallEntry();
    }
    return;
  }

  if (ports_.pin_is_default && ports_.pin_is_default()) {
    StartupMark("deferred_silent_unlock_begin");
    RunUnlockAndReadyAsync(std::string(kDefaultProfilePin), /*set_default_pin=*/false,
                           /*clear_default_pin=*/false);
    return;
  }
#ifdef PP_BROWSER_DEV_PIN
  // Local dev builds only (-DPP_BROWSER_DEV_PIN=…): same deferred path as a typed PIN, no prompt.
  // Deliberately not the Bootstrap-time unlock (--pin / PP_BROWSER_PIN): unlocking before the
  // first present starts the mesh ahead of the UI and hangs iOS before the first frame.
  if (PP_BROWSER_DEV_PIN[0] != '\0') {
    StartupMark("deferred_dev_pin_unlock_begin");
    RunUnlockAndReadyAsync(std::string(PP_BROWSER_DEV_PIN), /*set_default_pin=*/false,
                           /*clear_default_pin=*/false);
    return;
  }
#endif

  RequestShowUnlock(nullptr);
}

void ProfileUnlockGate::CompleteWithPin(const std::string& pin, const bool create_mode) {
  RunUnlockAndReadyAsync(pin, /*set_default_pin=*/false, /*clear_default_pin=*/create_mode);
}

void ProfileUnlockGate::CompleteWithDefaultPin() {
  RunUnlockAndReadyAsync(std::string(kDefaultProfilePin), /*set_default_pin=*/true,
                         /*clear_default_pin=*/false);
}

void ProfileUnlockGate::CompleteRestoreBackup(const std::string& pin, const bool set_default_pin) {
  if (unlock_in_progress_) {
    return;
  }
  if (!ports_.restore_local_backup) {
    ReportError("Account restore is not available");
    return;
  }
  SetUnlockInProgress(true);
  StartupMark("local_backup_restore_begin");

  auto wrong_pin = std::make_shared<bool>(false);
  auto work = [this, pin, wrong_pin]() -> Roe<void> {
    if (auto restored = ports_.restore_local_backup(pin); !restored) {
      *wrong_pin = AppError::CategoryOf(restored.error()) == ErrorCategory::Pin;
      return restored.error();
    }
    return UnlockAndReady(pin);
  };
  auto on_done = [this, set_default_pin, wrong_pin](Roe<void> result) {
    if (!result && *wrong_pin && ports_.ui.show_restore_pin) {
      // Not the default PIN: ask for the account's PIN; a typed PIN that is wrong keeps the page up.
      StartupMark("local_backup_restore_pin");
      SetUnlockInProgress(false);
      ports_.ui.show_restore_pin(/*wrong_pin=*/!set_default_pin);
      return;
    }
    if (!result) {
      StartupMark("local_backup_restore_fail");
      ReportError(result.error());
      return;
    }
    if (ports_.set_pin_is_default) {
      ports_.set_pin_is_default(set_default_pin);
    }
    StartupMark("local_backup_restore_ok");
    if (set_default_pin && ports_.ui.on_default_provisioned) {
      ports_.ui.on_default_provisioned();
    }
    if (ports_.ui.on_link_imported) {
      ports_.ui.on_link_imported(); // a restored account needs no wizard either
    }
    Finish(true);
  };

  if (ports_.run_heavy) {
    ports_.run_heavy(std::move(work), std::move(on_done));
    return;
  }
  on_done(work());
}

void ProfileUnlockGate::CompleteLinkDevice(const std::string& pin, const std::string& bundle_json,
                                           const bool set_default_pin) {
  if (!ports_.import_link_device) {
    ReportError("Link import is not available");
    return;
  }
  CompleteAccountImport(
      pin, set_default_pin, [this, pin, bundle_json]() { return ports_.import_link_device(pin, bundle_json); },
      "link_device_import");
}

void ProfileUnlockGate::CompletePhraseRestore(const std::string& pin, const std::string& phrase,
                                              const bool set_default_pin) {
  if (!ports_.restore_recovery_phrase) {
    ReportError("Restore from a recovery phrase is not available");
    return;
  }
  CompleteAccountImport(
      pin, set_default_pin, [this, pin, phrase]() { return ports_.restore_recovery_phrase(pin, phrase); },
      "phrase_restore");
}

void ProfileUnlockGate::CompleteAccountImport(const std::string& pin, const bool set_default_pin,
                                              std::function<Roe<void>()> import, const char* mark) {
  if (unlock_in_progress_) {
    return;
  }
  SetUnlockInProgress(true);
  const std::string mark_name = mark;
  StartupMark((mark_name + "_begin").c_str());

  auto work = [this, pin, import = std::move(import)]() -> Roe<void> {
    if (auto imported = import(); !imported) {
      return imported.error();
    }
    return UnlockAndReady(pin);
  };
  auto on_done = [this, set_default_pin, mark_name](Roe<void> result) {
    if (!result) {
      StartupMark((mark_name + "_fail").c_str());
      ReportError(result.error());
      return;
    }
    if (set_default_pin && ports_.set_pin_is_default) {
      ports_.set_pin_is_default(true);
    }
    if (!set_default_pin && ports_.set_pin_is_default) {
      ports_.set_pin_is_default(false);
    }
    StartupMark((mark_name + "_ok").c_str());
    if (set_default_pin && ports_.ui.on_default_provisioned) {
      ports_.ui.on_default_provisioned();
    }
    if (ports_.ui.on_link_imported) {
      ports_.ui.on_link_imported();
    }
    Finish(true);
  };

  if (ports_.run_heavy) {
    ports_.run_heavy(std::move(work), std::move(on_done));
    return;
  }
  on_done(work());
}

void ProfileUnlockGate::Cancel() {
  if (secrets_ && !Secrets().HasVault()) {
    // No account yet: PP is not usable without one, so "back" on the PIN / paste page returns to
    // the identity fork instead of dropping the user on an empty home screen (Kenneth 2026-10-13).
    showing_ = false;
    RequestShowFreshInstallEntry();
    return;
  }
  Finish(false);
}

} // namespace pbr
