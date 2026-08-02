#include "base/crypto/ProfileUnlockGate.h"

#include "base/crypto/PinDefaults.h"
#include "common/Logger.h"
#include "common/StartupTiming.h"

#include <stdexcept>
#include <utility>

namespace pbr {

Roe<void> UnlockProfileSecretsAndReady(ProfileSecretsService& secrets, const std::string& pin,
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

void ProfileUnlockGate::BindSecrets(ProfileSecretsService& secrets) {
  secrets_ = &secrets;
}

void ProfileUnlockGate::BindPorts(ProfileUnlockPorts ports) {
  ports_ = std::move(ports);
}

ProfileSecretsService& ProfileUnlockGate::Secrets() {
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
  logging::getLogger("ProfileUnlockGate").warning << "Unlock failed: " << message;
  SetUnlockInProgress(false);
  if (!showing_) {
    // Silent unlock failed with no modal yet — open unlock so the user can retry.
    RequestShowUnlock(nullptr);
  }
  if (ports_.ui.show_error) {
    ports_.ui.show_error(message);
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
      ReportError(result.error().message);
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
    RequestShowChooser(std::move(done));
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
      // No vault yet — leave locked until a feature calls EnsureUnlocked (chooser).
      DrainQueue(false);
    }
    return;
  }

  if (ports_.pin_is_default && ports_.pin_is_default()) {
    StartupMark("deferred_silent_unlock_begin");
    RunUnlockAndReadyAsync(std::string(kDefaultProfilePin), /*set_default_pin=*/false,
                           /*clear_default_pin=*/false);
    return;
  }

  RequestShowUnlock(nullptr);
}

void ProfileUnlockGate::CompleteWithPin(const std::string& pin, const bool create_mode) {
  RunUnlockAndReadyAsync(pin, /*set_default_pin=*/false, /*clear_default_pin=*/create_mode);
}

void ProfileUnlockGate::CompleteWithDefaultPin() {
  RunUnlockAndReadyAsync(std::string(kDefaultProfilePin), /*set_default_pin=*/true,
                         /*clear_default_pin=*/false);
}

void ProfileUnlockGate::Cancel() {
  Finish(false);
}

} // namespace pbr
