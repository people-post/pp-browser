#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/IDekConsumer.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Profile PIN vault and DEK fan-out — app-owned; independent of messaging.
 * Owned by Application (desktop/mobile) and NodeBootstrap (headless); not a singleton.
 */
class ProfileSecretsEngine {
public:
  ProfileSecretsEngine() = default;

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

  /** Re-fan the currently unlocked vault DEK (after link-device wrap of a shared DEK). */
  Roe<void> RedistributeUnlockedDek();

  DataKeyVault* Vault();
  const DataKeyVault* Vault() const;

  const std::string& ProfileDataDir() const { return profile_data_dir_; }
  const std::string& ProfileId() const { return profile_id_; }

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
