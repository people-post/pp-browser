#include "foundation/crypto/ProfileSecretsEngine.h"

#include "foundation/error/AppError.h"

#include <algorithm>
#include <filesystem>
#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {

Roe<void> ProfileSecretsEngine::Initialize(const std::string& profile_data_dir) {
  if (initialized_) {
    return {};
  }

  profile_data_dir_ = profile_data_dir;
  profile_id_ = std::filesystem::path(profile_data_dir_).filename().string();
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }

  vault_ = std::make_unique<DataKeyVault>(DataKeyVault::VaultPathForProfile(profile_data_dir_), profile_id_);
  unlocked_ = false;
  initialized_ = true;
  return {};
}

void ProfileSecretsEngine::Shutdown() {
  if (!initialized_) {
    return;
  }
  ClearDekConsumers();
  if (vault_) {
    vault_->Lock();
    vault_.reset();
  }
  unlocked_ = false;
  initialized_ = false;
  on_unlocked_ = nullptr;
}

bool ProfileSecretsEngine::HasVault() const {
  return vault_ && vault_->Exists();
}

bool ProfileSecretsEngine::NeedsUnlock() const {
  return initialized_ && HasVault() && !unlocked_;
}

void ProfileSecretsEngine::SetOnUnlocked(std::function<void()> callback) {
  on_unlocked_ = std::move(callback);
}

void ProfileSecretsEngine::RegisterDekConsumer(IDekConsumer* consumer) {
  if (consumer == nullptr) {
    return;
  }
  for (IDekConsumer* existing : dek_consumers_) {
    if (existing == consumer) {
      return;
    }
  }
  dek_consumers_.push_back(consumer);
}

void ProfileSecretsEngine::UnregisterDekConsumer(IDekConsumer* consumer) {
  if (consumer == nullptr) {
    return;
  }
  dek_consumers_.erase(
      std::remove(dek_consumers_.begin(), dek_consumers_.end(), consumer), dek_consumers_.end());
}

Roe<void> ProfileSecretsEngine::DistributeDek(const ByteVector& dek) {
  for (IDekConsumer* consumer : dek_consumers_) {
    if (consumer == nullptr) {
      continue;
    }
    if (auto set = consumer->SetDek(dek); !set) {
      return set.error();
    }
  }
  return {};
}

void ProfileSecretsEngine::ClearDekConsumers() {
  for (IDekConsumer* consumer : dek_consumers_) {
    if (consumer != nullptr) {
      consumer->ClearDek();
    }
  }
  dek_consumers_.clear();
}

void ProfileSecretsEngine::NotifyUnlocked() {
  if (on_unlocked_) {
    on_unlocked_();
  }
}

Roe<void> ProfileSecretsEngine::Unlock(const SecretText& pin) {
  if (!initialized_) {
    return AppError::Pin(Err::Pin::VaultUnavailable, "Profile secrets service not initialized");
  }
  if (unlocked_) {
    return {};
  }
  if (pin.empty()) {
    return AppError::Pin(Err::Pin::Required, "PIN is required");
  }
  if (!vault_) {
    vault_ = std::make_unique<DataKeyVault>(DataKeyVault::VaultPathForProfile(profile_data_dir_), profile_id_);
  }

  if (!vault_->Exists()) {
    if (auto created = vault_->Create(pin.Reveal()); !created) {
      return created.error();
    }
  } else if (!vault_->IsUnlocked()) {
    if (auto unlocked = vault_->Unlock(pin.Reveal()); !unlocked) {
      return unlocked.error();
    }
  }

  auto dek = vault_->Dek();
  if (!dek) {
    return dek.error();
  }
  if (auto distributed = DistributeDek(*dek); !distributed) {
    Lock();
    return distributed.error();
  }

  unlocked_ = true;
  NotifyUnlocked();
  return {};
}

Roe<void> ProfileSecretsEngine::RedistributeUnlockedDek() {
  if (!initialized_ || !vault_ || !vault_->IsUnlocked()) {
    return AppError::Pin(Err::Pin::VaultUnavailable, "Profile vault is not unlocked");
  }
  auto dek = vault_->Dek();
  if (!dek) {
    return dek.error();
  }
  if (auto distributed = DistributeDek(*dek); !distributed) {
    return distributed.error();
  }
  unlocked_ = true;
  NotifyUnlocked();
  return {};
}

void ProfileSecretsEngine::AttachGrace(std::unique_ptr<GraceUnlock> grace, std::function<GraceClock()> clock) {
  grace_ = std::move(grace);
  grace_clock_ = std::move(clock);
}

bool ProfileSecretsEngine::GraceSupported() const {
  return grace_ && grace_clock_ && grace_->Supported();
}

Roe<GraceUnlock::Result> ProfileSecretsEngine::TryGraceUnlock() {
  GraceUnlock::Result missing;
  if (!initialized_ || !GraceSupported()) {
    return missing;
  }
  auto found = grace_->TryUnlock(grace_clock_());
  if (!found) {
    return found.error();
  }
  // The result carries the DEK in the clear and callers only read `status`: wipe it on every way out.
  auto wipe_dek = [&found]() {
    if (!found->dek.empty()) {
      sodium_memzero(found->dek.data(), found->dek.size());
      found->dek.clear();
    }
  };
  if (found->status != GraceUnlock::Status::Found || unlocked_) {
    wipe_dek();
    return found;
  }
  if (!vault_ || !vault_->Exists()) {
    wipe_dek();
    grace_->Forget();
    return missing;
  }
  if (auto set = vault_->UnlockWithDek(found->dek); !set) {
    wipe_dek();
    grace_->Forget();
    return missing;
  }
  wipe_dek();
  // The same tail as Unlock(): fan the DEK out, mark unlocked, tell the listener.
  if (auto distributed = RedistributeUnlockedDek(); !distributed) {
    Lock();
    return distributed.error();
  }
  return found;
}

Roe<void> ProfileSecretsEngine::RememberGrace(const int minutes) {
  if (!GraceSupported()) {
    return {};
  }
  if (!vault_ || !vault_->IsUnlocked()) {
    return AppError::Pin(Err::Pin::VaultUnavailable, "Profile vault is not unlocked");
  }
  auto dek = vault_->Dek();
  if (!dek) {
    return dek.error();
  }
  auto remembered = grace_->Remember(*dek, minutes, grace_clock_());
  sodium_memzero(dek->data(), dek->size());
  return remembered;
}

Roe<void> ProfileSecretsEngine::TouchGrace(const int minutes) {
  if (!GraceSupported()) {
    return {};
  }
  return grace_->Touch(minutes, grace_clock_());
}

void ProfileSecretsEngine::ForgetGrace() {
  if (grace_) {
    grace_->Forget();
  }
}

void ProfileSecretsEngine::AttachBiometric(std::unique_ptr<BiometricUnlock> biometric) {
  biometric_ = std::move(biometric);
}

bool ProfileSecretsEngine::BiometricSupported() const {
  return biometric_ && biometric_->Supported();
}

bool ProfileSecretsEngine::BiometricEnabled() const {
  return BiometricSupported() && biometric_->Enabled();
}

Roe<BiometricUnlock::Result> ProfileSecretsEngine::TryBiometricUnlock(const std::string& prompt) {
  BiometricUnlock::Result missing;
  if (!initialized_ || !BiometricSupported()) {
    return missing;
  }
  auto found = biometric_->TryUnlock(prompt);
  if (!found) {
    return found.error();
  }
  // The same handling as TryGraceUnlock: the DEK is wiped on every way out. Nothing on disk can check it against
  // the vault without the PIN, so the vault's own size check is the check; the DEK-replacing paths Disable.
  auto wipe_dek = [&found]() {
    if (!found->dek.empty()) {
      sodium_memzero(found->dek.data(), found->dek.size());
      found->dek.clear();
    }
  };
  if (found->status != BiometricUnlock::Status::Found || unlocked_) {
    wipe_dek();
    return found;
  }
  if (!vault_ || !vault_->Exists()) {
    wipe_dek();
    biometric_->Disable();
    return missing;
  }
  if (auto set = vault_->UnlockWithDek(found->dek); !set) {
    wipe_dek();
    biometric_->Disable();
    return missing;
  }
  wipe_dek();
  if (auto distributed = RedistributeUnlockedDek(); !distributed) {
    Lock();
    return distributed.error();
  }
  return found;
}

Roe<void> ProfileSecretsEngine::EnableBiometric(std::string_view pin) {
  if (!initialized_ || !vault_) {
    return AppError::Pin(Err::Pin::VaultUnavailable, "Profile secrets service not initialized");
  }
  if (!BiometricSupported()) {
    return Error("Biometric unlock is not supported on this device");
  }
  auto bytes = vault_->ReadVaultFile();
  if (!bytes) {
    return bytes.error();
  }
  auto dek = DataKeyVault::UnwrapDekFromVaultBytes(*bytes, vault_->ProfileId(), pin);
  if (!dek) {
    return dek.error();
  }
  auto enabled = biometric_->Enable(*dek);
  sodium_memzero(dek->data(), dek->size());
  return enabled;
}

void ProfileSecretsEngine::DisableBiometric() {
  if (biometric_) {
    biometric_->Disable();
  }
}

Roe<void> ProfileSecretsEngine::VerifyPin(std::string_view pin) const {
  if (!initialized_ || !vault_) {
    return AppError::Pin(Err::Pin::VaultUnavailable, "Profile secrets service not initialized");
  }
  // The KEK derivation and DEK unwrap Unlock() does, on the file alone: dek_ and the consumers stay as they are.
  auto bytes = vault_->ReadVaultFile();
  if (!bytes) {
    return bytes.error();
  }
  auto dek = DataKeyVault::UnwrapDekFromVaultBytes(*bytes, vault_->ProfileId(), pin);
  if (!dek) {
    return dek.error();
  }
  sodium_memzero(dek->data(), dek->size());
  return {};
}

Roe<void> ProfileSecretsEngine::ChangePin(std::string_view current_pin, std::string_view new_pin) {
  if (!initialized_ || !vault_) {
    return AppError::Pin(Err::Pin::VaultUnavailable, "Profile secrets service not initialized");
  }
  if (auto changed = vault_->ChangePin(current_pin, new_pin); !changed) {
    return changed.error();
  }
  ForgetGrace();
  return {};
}

void ProfileSecretsEngine::Lock() {
  ClearDekConsumers();
  if (vault_) {
    vault_->Lock();
  }
  unlocked_ = false;
}

DataKeyVault* ProfileSecretsEngine::Vault() {
  return vault_.get();
}

const DataKeyVault* ProfileSecretsEngine::Vault() const {
  return vault_.get();
}

} // namespace pbr
