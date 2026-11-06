#include "foundation/crypto/ProfileUnlockGate.h"

#include "common/Diag.h"
#include "foundation/crypto/PinDefaults.h"
#include "foundation/error/AppError.h"
#include "common/Localized.h"
#include "common/StartupTiming.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

ProfileUnlockGate::ProfileUnlockGate() {
  redirectLogger("ProfileUnlockGate");
}

Roe<void> UnlockProfileSecretsAndReady(ProfileSecretsEngine& secrets, const SecretText& pin,
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
  // The handler registered through the old ports is removed through them.
  if (protected_data_handler_ && ports_.remove_protected_data_handler) {
    ports_.remove_protected_data_handler(*protected_data_handler_);
  }
  protected_data_handler_.reset();
  grace_pending_ = false;
  waiting_device_unlock_ = false;
  ++device_unlock_wait_generation_;
  ports_ = std::move(ports);
}

ProfileSecretsEngine& ProfileUnlockGate::Secrets() {
  if (!secrets_) {
    throw std::runtime_error("ProfileUnlockGate secrets not bound");
  }
  return *secrets_;
}

Roe<void> ProfileUnlockGate::UnlockAndReady(const SecretText& pin) {
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
  if (in_progress) {
    DiagLine("unlock.start").Emit();
  }
  if (ports_.ui.set_unlock_in_progress) {
    ports_.ui.set_unlock_in_progress(in_progress);
  }
}

void ProfileUnlockGate::Finish(const bool unlocked) {
  StopGrace();
  DiagLine("unlock.done").Add("unlocked", unlocked).Emit();
  SetUnlockInProgress(false);
  if (ports_.ui.dismiss) {
    ports_.ui.dismiss();
  }
  DrainQueue(unlocked);
}

void ProfileUnlockGate::ReportError(const std::string& message) {
  log().warning << "Unlock failed: " << message;
  DiagLine("unlock.failed").Add("wrong_pin", false).Add("silent", !showing_).Emit();
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
    DiagLine("unlock.error").Fail("error", error).Emit();
    ReportError(error.message);
    return;
  }
  log().warning << "Unlock failed: " << error.message;
  DiagLine("unlock.failed").Add("wrong_pin", true).Add("silent", !showing_).Emit();
  SetUnlockInProgress(false);
  const bool silent = !showing_;
  if (silent) {
    RequestShowUnlock(nullptr);
  }
  if (!silent && ports_.ui.show_error) {
    ports_.ui.show_error(Localized("pin.error.wrong"));
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

void ProfileUnlockGate::RunUnlockAndReadyAsync(SecretText pin, const bool set_default_pin,
                                               const bool clear_default_pin, const bool remember_grace) {
  if (unlock_in_progress_) {
    return;
  }
  SetUnlockInProgress(true);
  StartupMark("unlock_and_ready_begin");

  auto work = [this, pin = std::move(pin)]() -> Roe<void> { return UnlockAndReady(pin); };
  auto on_done = [this, set_default_pin, clear_default_pin, remember_grace](Roe<void> result) {
    if (!result) {
      StartupMark("unlock_and_ready_fail");
      ReportError(result.error());
      return;
    }
    if (remember_grace) {
      RememberGraceAfterPinUnlock();
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
      // No vault yet (fresh install): go straight to "I'm new / I already have an account", so PIN
      // creation and the first-run wizard follow the first frame instead of an empty home screen.
      RequestShowIdentityFork(nullptr);
    }
    return;
  }

  if (ports_.pin_is_default && ports_.pin_is_default()) {
    StartupMark("deferred_silent_unlock_begin");
    RunUnlockAndReadyAsync(std::string(kDefaultProfilePin), /*set_default_pin=*/false,
                           /*clear_default_pin=*/false, /*remember_grace=*/false);
    return;
  }
  // PIN grace window (#21 phase 1): a PIN typed a short while ago opens the vault again without asking; before the
  // dev PIN, so a dev build exercises it too.
  if (ports_.try_grace_unlock && ports_.grace_minutes && ports_.grace_minutes() > 0) {
    StartupMark("deferred_grace_unlock_begin");
    RunGraceAttempt(GraceStep::First);
    return;
  }
  UnlockWithoutGrace();
}

void ProfileUnlockGate::UnlockWithoutGrace() {
#ifdef PP_BROWSER_DEV_PIN
  // Local dev builds only (-DPP_BROWSER_DEV_PIN=…): same deferred path as a typed PIN, no prompt.
  // Deliberately not the Bootstrap-time unlock (--pin / PP_BROWSER_PIN): unlocking before the
  // first present starts the mesh ahead of the UI and hangs iOS before the first frame.
  if (PP_BROWSER_DEV_PIN[0] != '\0') {
    StartupMark("deferred_dev_pin_unlock_begin");
    RunUnlockAndReadyAsync(std::string(PP_BROWSER_DEV_PIN), /*set_default_pin=*/false,
                           /*clear_default_pin=*/false, /*remember_grace=*/true);
    return;
  }
#endif

  RequestShowUnlock(nullptr);
}

void ProfileUnlockGate::CompleteWithPin(const SecretText& pin, const bool create_mode) {
  RunUnlockAndReadyAsync(pin, /*set_default_pin=*/false, /*clear_default_pin=*/create_mode, /*remember_grace=*/true);
}

void ProfileUnlockGate::CompleteWithDefaultPin() {
  RunUnlockAndReadyAsync(std::string(kDefaultProfilePin), /*set_default_pin=*/true,
                         /*clear_default_pin=*/false, /*remember_grace=*/false);
}

void ProfileUnlockGate::CompleteLinkDevice(const SecretText& pin, const std::string& bundle_json,
                                           const bool set_default_pin) {
  if (!ports_.import_link_device) {
    ReportError("Link import is not available");
    return;
  }
  CompleteAccountImport(
      pin, set_default_pin, [this, pin, bundle_json]() { return ports_.import_link_device(pin, bundle_json); },
      "link_device_import");
}

void ProfileUnlockGate::CompletePhraseRestore(const SecretText& pin, const SecretText& phrase,
                                              const bool set_default_pin, std::vector<std::string> saved_copies) {
  if (!ports_.restore_recovery_phrase) {
    ReportError("Restore from a recovery phrase is not available");
    return;
  }
  CompleteAccountImport(
      pin, set_default_pin,
      [this, pin, phrase, saved_copies = std::move(saved_copies)]() {
        return ports_.restore_recovery_phrase(pin, phrase, saved_copies);
      },
      "phrase_restore");
}

void ProfileUnlockGate::CompleteAccountImport(const SecretText& pin, const bool set_default_pin,
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
    if (!set_default_pin) {
      RememberGraceAfterPinUnlock();
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
    RequestShowIdentityFork(nullptr);
    return;
  }
  Finish(false);
}

// --- PIN grace window (#21 phase 1, DEV/PLAN-pin-grace.md) ---

void ProfileUnlockGate::SetGraceAttemptInProgress(const bool in_progress) {
  // Holds EnsureUnlocked callers and a typed PIN back like any unlock; unlock.start stays for a PIN or the
  // default PIN (a grace attempt says unlock.grace).
  unlock_in_progress_ = in_progress;
  if (ports_.ui.set_unlock_in_progress) {
    ports_.ui.set_unlock_in_progress(in_progress);
  }
}

void ProfileUnlockGate::RunGraceAttempt(const GraceStep step) {
  SetGraceAttemptInProgress(true);
  auto status = std::make_shared<std::optional<GraceUnlock::Status>>();
  auto work = [this, status]() -> Roe<void> {
    auto got = ports_.try_grace_unlock();
    if (!got) {
      return got.error();
    }
    *status = got->status;
    if (got->status != GraceUnlock::Status::Found || !ports_.ensure_messaging_ready) {
      return {};
    }
    return ports_.ensure_messaging_ready();
  };
  auto on_done = [this, step, status](Roe<void> result) { OnGraceAttemptDone(step, *status, std::move(result)); };
  if (ports_.run_heavy) {
    ports_.run_heavy(std::move(work), std::move(on_done));
    return;
  }
  on_done(work());
}

void ProfileUnlockGate::OnGraceAttemptDone(const GraceStep step, const std::optional<GraceUnlock::Status> status,
                                           Roe<void> result) {
  SetGraceAttemptInProgress(false);
  if (status == GraceUnlock::Status::Found) {
    DiagLine("unlock.grace").Add("result", "hit").Emit();
    if (!result) {
      // The vault is open but messaging did not start: as after a PIN unlock that got this far.
      StopGrace();
      StartupMark("grace_unlock_fail");
      ReportError(result.error());
      return;
    }
    StartupMark("grace_unlock_ok");
    Finish(true);
    return;
  }
  if (status == GraceUnlock::Status::Unavailable) {
    DiagLine("unlock.grace").Add("result", "unavailable").Emit();
    OnGraceUnavailable(step);
    return;
  }
  if (!result) {
    log().warning << "Grace entry not read: " << result.error().message;
    DiagLine("unlock.grace").Add("result", "error").Fail("error", result.error()).Emit();
  } else if (status == GraceUnlock::Status::Expired) {
    DiagLine("unlock.grace").Add("result", "expired").Emit();
  } else {
    DiagLine("unlock.grace").Add("result", "missing").Emit();
  }
  // No window to wait for: the PIN, as without one.
  StopGrace();
  if (step != GraceStep::Retry) {
    UnlockWithoutGrace();
  }
  // A retry runs with the unlock screen already up; it stays.
}

void ProfileUnlockGate::OnGraceUnavailable(const GraceStep step) {
  switch (step) {
  case GraceStep::First:
    // Register first, then read once more: the phone may be unlocked between the two (the handler would not fire).
    if (ports_.on_protected_data_available && !protected_data_handler_) {
      protected_data_handler_ = ports_.on_protected_data_available([this]() {
        if (ports_.run_on_ui) {
          ports_.run_on_ui([this]() { RetryGrace(); });
          return;
        }
        RetryGrace();
      });
    }
    RunGraceAttempt(GraceStep::Recheck);
    return;
  case GraceStep::Recheck:
    grace_pending_ = true;
    RequestShowUnlock(nullptr);
    if (ports_.has_pending_push_call && ports_.has_pending_push_call()) {
      // A call answered on the locked phone: it connects as soon as the phone is unlocked, no PIN.
      waiting_device_unlock_ = true;
      if (ports_.ui.show_waiting_device_unlock) {
        ports_.ui.show_waiting_device_unlock(true);
      }
      if (ports_.run_later) {
        const uint64_t generation = device_unlock_wait_generation_;
        ports_.run_later(kDeviceUnlockWait, [this, generation]() {
          if (generation == device_unlock_wait_generation_) {
            EndDeviceUnlockWait();
          }
        });
      }
    }
    // Without a call: the plain unlock screen; the phone's unlock or the next foreground may still open it.
    return;
  case GraceStep::Retry:
    // Still locked: a call stops waiting (the PIN screen), the handler stays for a later unlock.
    EndDeviceUnlockWait();
    return;
  }
}

void ProfileUnlockGate::RetryGrace() {
  if (!grace_pending_ || unlock_in_progress_ || (secrets_ && Secrets().IsUnlocked())) {
    return;
  }
  RunGraceAttempt(GraceStep::Retry);
}

void ProfileUnlockGate::OnForeground() {
  if (!grace_pending_) {
    return;
  }
  if (ports_.protected_data_available && !ports_.protected_data_available()) {
    return;
  }
  RetryGrace();
}

void ProfileUnlockGate::UseDevicePinInstead() {
  EndDeviceUnlockWait();
}

void ProfileUnlockGate::EndDeviceUnlockWait() {
  ++device_unlock_wait_generation_;
  if (!waiting_device_unlock_) {
    return;
  }
  waiting_device_unlock_ = false;
  if (ports_.ui.show_waiting_device_unlock) {
    ports_.ui.show_waiting_device_unlock(false);
  }
}

void ProfileUnlockGate::StopGrace() {
  EndDeviceUnlockWait();
  grace_pending_ = false;
  if (protected_data_handler_) {
    if (ports_.remove_protected_data_handler) {
      ports_.remove_protected_data_handler(*protected_data_handler_);
    }
    protected_data_handler_.reset();
  }
}

void ProfileUnlockGate::RememberGraceAfterPinUnlock() {
  if (!ports_.remember_grace || !ports_.grace_minutes) {
    return;
  }
  // Never fails the unlock: without the entry the next cold start asks for the PIN, as before.
  if (auto kept = ports_.remember_grace(ports_.grace_minutes()); !kept) {
    log().warning << "Grace window not kept: " << kept.error().message;
    DiagLine("unlock.grace").Add("result", "write_failed").Fail("error", kept.error()).Emit();
  }
}

} // namespace pbr
