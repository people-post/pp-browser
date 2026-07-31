#include "base/crypto/ProfileUnlockGate.h"

#include "base/crypto/PinDefaults.h"
#include "common/StartupTiming.h"

#include <stdexcept>
#include <utility>

namespace pbr {

Roe<void> UnlockProfileSecretsAndReady(ProfileSecretsService& secrets, const std::string& pin,
                                       const std::function<Roe<void>()>& ensure_messaging_ready) {
  if (auto unlocked = secrets.Unlock(pin); !unlocked) {
    return unlocked.error();
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

bool ProfileUnlockGate::TrySilentDefaultUnlock() {
  StartupPhase phase("ProfileUnlockGate::TrySilentDefaultUnlock");
  if (!ports_.pin_is_default || !ports_.pin_is_default()) {
    return false;
  }
  if (ports_.messaging_ready && ports_.messaging_ready()) {
    return true;
  }
  if (!Secrets().HasVault()) {
    return false;
  }
  if (!Secrets().IsUnlocked()) {
    if (!Secrets().Unlock(kDefaultProfilePin)) {
      return false;
    }
  }
  if (!ports_.ensure_messaging_ready) {
    return Secrets().IsUnlocked();
  }
  return static_cast<bool>(ports_.ensure_messaging_ready());
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
    if (auto ready = ports_.ensure_messaging_ready(); ready) {
      if (done) {
        done(true);
      }
    } else if (done) {
      done(false);
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
      const bool ready =
          ports_.ensure_messaging_ready ? static_cast<bool>(ports_.ensure_messaging_ready()) : false;
      DrainQueue(ready);
    } else {
      // No vault yet — leave locked until a feature calls EnsureUnlocked (chooser).
      DrainQueue(false);
    }
    return;
  }

  if (ports_.pin_is_default && ports_.pin_is_default()) {
    SetUnlockInProgress(true);
    StartupMark("deferred_silent_unlock_begin");
    const bool ok = TrySilentDefaultUnlock();
    SetUnlockInProgress(false);
    StartupMark(ok ? "deferred_silent_unlock_ok" : "deferred_silent_unlock_fail");
    if (ok) {
      DrainQueue(true);
      return;
    }
  }

  RequestShowUnlock(nullptr);
}

Roe<void> ProfileUnlockGate::CompleteWithPin(const std::string& pin, const bool create_mode) {
  if (auto unlocked = UnlockAndReady(pin); !unlocked) {
    return unlocked.error();
  }
  if (create_mode && ports_.set_pin_is_default) {
    ports_.set_pin_is_default(false);
  }
  Finish(true);
  return {};
}

Roe<void> ProfileUnlockGate::CompleteWithDefaultPin() {
  if (auto unlocked = UnlockAndReady(kDefaultProfilePin); !unlocked) {
    return unlocked.error();
  }
  if (ports_.set_pin_is_default) {
    ports_.set_pin_is_default(true);
  }
  Finish(true);
  return {};
}

void ProfileUnlockGate::Cancel() {
  Finish(false);
}

} // namespace pbr
