#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/DataKeyVault.h"
#include "base/crypto/IDekConsumer.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Profile PIN vault and DEK fan-out — app-owned; independent of messaging.
 * Owned by Application (desktop/mobile) and NodeBootstrap (headless); not a singleton.
 */
class ProfileSecretsService {
public:
  ProfileSecretsService() = default;

  Roe<void> Initialize(const std::string& profile_data_dir);
  void Shutdown();
  bool IsInitialized() const { return initialized_; }

  bool HasVault() const;
  /** Vault exists on disk but DEK not yet distributed to consumers. */
  bool NeedsUnlock() const;
  bool IsUnlocked() const { return unlocked_; }

  /** Create vault (if missing) or unlock with PIN, then fan out DEK to consumers. */
  Roe<void> Unlock(const std::string& pin);
  void Lock();

  DataKeyVault* Vault();
  const DataKeyVault* Vault() const;

  void RegisterDekConsumer(IDekConsumer* consumer);
  void UnregisterDekConsumer(IDekConsumer* consumer);

  void SetOnUnlocked(std::function<void()> callback);

private:
  Roe<void> DistributeDek(const ByteVector& dek);
  void ClearDekConsumers();
  void NotifyUnlocked();

  std::string profile_data_dir_;
  std::string profile_id_;
  std::unique_ptr<DataKeyVault> vault_;
  std::vector<IDekConsumer*> dek_consumers_;
  std::function<void()> on_unlocked_;
  bool initialized_ = false;
  bool unlocked_ = false;
};

} // namespace pbr
