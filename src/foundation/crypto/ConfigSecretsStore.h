#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IDekConsumer.h"
#include "foundation/data/ConfigSecrets.h"

#include <functional>
#include <mutex>
#include <string>

namespace pbr {

class ProfileSecretsEngine;

/**
 * `{profile}/secrets.enc`: the typed model API key under the profile DEK (FileCipher, AAD purpose
 * "config-secrets"), schema_version 1 (PB-034). A DEK consumer like IdentityStore: Ready() once the
 * vault is unlocked. The file is removed when there is nothing to keep.
 */
class ConfigSecretsStore : public IDekConsumer, public IConfigSecrets {
public:
  explicit ConfigSecretsStore(const ProfileSecretsEngine& engine);
  ~ConfigSecretsStore() override;

  /** Runs right after the DEK arrives (SessionStore::ApplyConfigSecrets hangs here). */
  void SetOnReady(std::function<void()> callback);

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  bool Ready() const override;
  Roe<ConfigSecrets> Load() const override;
  Roe<void> Store(const ConfigSecrets& secrets) override;

  static std::string PathFor(const std::string& profile_data_dir);
  /** Pure codec, for tests and Load/Store. */
  static Roe<ByteVector> Seal(const ByteVector& dek, const std::string& profile_id, const ConfigSecrets& secrets);
  static Roe<ConfigSecrets> Open(const ByteVector& dek, const std::string& profile_id, const ByteVector& blob);

private:
  const ProfileSecretsEngine& engine_;
  std::function<void()> on_ready_;
  mutable std::mutex mutex_;
  ByteVector dek_;
};

} // namespace pbr
