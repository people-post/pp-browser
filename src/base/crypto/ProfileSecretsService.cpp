#include "base/crypto/ProfileSecretsService.h"

#include "base/error/AppError.h"

#include <algorithm>
#include <filesystem>

namespace pbr {

Roe<void> ProfileSecretsService::Initialize(const std::string& profile_data_dir) {
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

void ProfileSecretsService::Shutdown() {
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

bool ProfileSecretsService::HasVault() const {
  return vault_ && vault_->Exists();
}

bool ProfileSecretsService::NeedsUnlock() const {
  return initialized_ && HasVault() && !unlocked_;
}

void ProfileSecretsService::SetOnUnlocked(std::function<void()> callback) {
  on_unlocked_ = std::move(callback);
}

void ProfileSecretsService::RegisterDekConsumer(IDekConsumer* consumer) {
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

void ProfileSecretsService::UnregisterDekConsumer(IDekConsumer* consumer) {
  if (consumer == nullptr) {
    return;
  }
  dek_consumers_.erase(
      std::remove(dek_consumers_.begin(), dek_consumers_.end(), consumer), dek_consumers_.end());
}

Roe<void> ProfileSecretsService::DistributeDek(const ByteVector& dek) {
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

void ProfileSecretsService::ClearDekConsumers() {
  for (IDekConsumer* consumer : dek_consumers_) {
    if (consumer != nullptr) {
      consumer->ClearDek();
    }
  }
}

void ProfileSecretsService::NotifyUnlocked() {
  if (on_unlocked_) {
    on_unlocked_();
  }
}

Roe<void> ProfileSecretsService::Unlock(const std::string& pin) {
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
    if (auto created = vault_->Create(pin); !created) {
      return created.error();
    }
  } else if (!vault_->IsUnlocked()) {
    if (auto unlocked = vault_->Unlock(pin); !unlocked) {
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

void ProfileSecretsService::Lock() {
  ClearDekConsumers();
  if (vault_) {
    vault_->Lock();
  }
  unlocked_ = false;
}

DataKeyVault* ProfileSecretsService::Vault() {
  return vault_.get();
}

const DataKeyVault* ProfileSecretsService::Vault() const {
  return vault_.get();
}

} // namespace pbr
