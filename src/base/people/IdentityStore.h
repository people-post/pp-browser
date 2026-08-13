#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "base/crypto/CryptoTypes.h"
#include "base/crypto/IDekConsumer.h"
#include "base/people/IdentityTypes.h"

#include <mutex>
#include <string>
#include <vector>

namespace pbr {

class IdentityStore : public Module, public IDekConsumer {
public:
  /** Current identity plaintext JSON schema inside identity.enc. Unversioned files migrate on load. */
  static constexpr int kSchemaVersion = 2;

  explicit IdentityStore(std::string data_dir, std::string profile_id = {});

  /** Required before LoadOrCreate/Get/Save — DEK from unlocked DataKeyVault. */
  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  Roe<LocalIdentity> LoadOrCreate();
  Roe<LocalIdentity> Get() const;
  Roe<LocalIdentity> Update(const LocalIdentity& identity);
  Roe<std::string> SignPayload(const std::string& canonical_json) const;
  Roe<std::string> SignBytes(const std::vector<uint8_t>& sign_bytes) const;
  /** Raw 32-byte Ed25519 private key for libp2p Host identity binding. */
  Roe<ByteVector> GetEd25519PrivateKey() const;
  Roe<ByteVector> GetEd25519PublicKey() const;
  Roe<ByteVector> GetOrCreateHybridKemPrivateKey() const;
  Roe<std::string> GetHybridKemPublicKeyB64() const;
  /** Account ML-DSA-65 secret (raw). Empty/error if not yet minted. */
  Roe<ByteVector> GetAccountMlDsaPrivateKey() const;
  Roe<std::string> GetAccountId() const;
  void Flush();

private:
  Roe<void> EnsureLoaded() const;
  Roe<void> Save() const;
  Roe<void> RequireDek() const;
  std::string StorePath() const;
  std::string ProfileId() const;

  std::string data_dir_;
  std::string profile_id_;
  mutable ByteVector dek_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable LocalIdentity identity_;
  mutable std::vector<uint8_t> private_key_;
  mutable bool dirty_ = false;
};

} // namespace pbr
