#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/PinKeyDeriver.h"

#include "common/Error.h"

#include <string>
#include <string_view>

namespace pbr {

/**
 * PIN-wrapped profile DEK vault (`vault.bin`).
 * Unlocked DEK lives in memory until Lock(); forgotten PIN → wipe profile (no recovery).
 */
class DataKeyVault {
public:
  explicit DataKeyVault(std::string vault_path, std::string profile_id);

  static std::string VaultPathForProfile(const std::string& profile_data_dir);
  static bool Exists(const std::string& vault_path);

  bool Exists() const;
  bool IsUnlocked() const { return !dek_.empty(); }
  const std::string& ProfileId() const { return profile_id_; }

  Roe<void> Create(std::string_view pin);
  Roe<void> Unlock(std::string_view pin);
  Roe<void> ChangePin(std::string_view old_pin, std::string_view new_pin);
  void Lock();

  /** Inject a DEK for tests without a vault file. */
  Roe<void> UnlockWithDek(ByteVector dek);

  Roe<ByteVector> Dek() const;

private:
  Roe<ByteVector> ReadVaultFile() const;
  Roe<void> WriteVaultFile(const PinKdfParams& params, const ByteVector& wrapped_dek) const;
  Roe<ByteVector> WrapDek(const ByteVector& kek, const ByteVector& dek) const;
  Roe<ByteVector> UnwrapDek(const ByteVector& kek, const ByteVector& wrapped) const;

  std::string vault_path_;
  std::string profile_id_;
  ByteVector dek_;
};

} // namespace pbr
