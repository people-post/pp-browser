#pragma once

#include "common/directory/IAccountSigningAccess.h"
#include "common/directory/ILocalAccount.h"
#include "common/Error.h"
#include "common/Module.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IDekConsumer.h"
#include "common/directory/IdentityTypes.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IdentityStore : public Module, public IDekConsumer, public IAccountSigningAccess, public ILocalAccount {
public:
  /**
   * Current identity plaintext JSON schema inside identity.enc. Unversioned / v1–v2 Ed25519 device keys fail closed
   * (PQ hard cut); 3 is stepped up on load (its `relay_user_id` dropped: the relay addresses the account by its
   * Account ID, multi-relay MR001).
   */
  static constexpr int kSchemaVersion = 4;
  /** The oldest identity.enc this build steps up (DATA_LAYOUT § Schema versions). */
  static constexpr int kFloorSchemaVersion = 3;

  explicit IdentityStore(std::string data_dir, std::string profile_id = {});

  /** Optional master identity seed (≥32 bytes). When set:
   *  - empty identity.enc → mint device/account/KEM from HKDF (pp-node-identity-v1)
   *  - existing identity.enc → fail closed if derived keys ≠ stored keys
   * Call before LoadOrCreate (pp-node: PP_NODE_IDENTITY_SEED).
   */
  Roe<void> SetIdentitySeed(ByteVector master_seed);

  /** Required before LoadOrCreate/Get/Save — DEK from unlocked DataKeyVault. */
  Roe<void> SetDek(ByteVector dek) override;
  /**
   * Swap the in-memory DEK without discarding a loaded identity (link-device
   * import). Caller must Update/Save next so identity.enc is re-sealed.
   */
  Roe<void> ReplaceDekKeepLoaded(ByteVector dek);
  void ClearDek() override;

  Roe<LocalIdentity> LoadOrCreate();
  /**
   * The recovery seed this account's keys derive from (M023 §2; its phrase: EncodeRecoveryPhrase). None for an
   * account minted before M023 or a pp-node.
   */
  Roe<std::optional<ByteVector>> RecoverySeed() const;
  Roe<LocalIdentity> Get() const;
  /** Writes `identity`; on a failed write the identity in memory stays as it was (nothing half-applied). */
  Roe<LocalIdentity> Update(const LocalIdentity& identity);
  /**
   * One read-modify-write under the store's lock: `change` edits the identity as stored now and returns whether to
   * save it (false: nothing is written). The stored copy in memory changes only once the write succeeded: a failed
   * save leaves Get() as it was. Two writers off different threads cannot overwrite each other's fields this
   * way, as Get + Update can. `change` must not call back into this store. Returns the identity after it.
   */
  Roe<LocalIdentity> Modify(const std::function<bool(LocalIdentity& identity)>& change);
  Roe<std::string> SignPayload(const std::string& canonical_json) const;
  Roe<std::string> SignBytes(const std::vector<uint8_t>& sign_bytes) const;
  Roe<std::string> SignAccountBytes(const AccountSignBytes& sign_bytes) const override;
  /** Signs with this device's own ML-DSA-65 key (its PeerId's; M021: the device signs its envelopes). */
  Roe<std::string> SignDeviceBytes(const std::vector<uint8_t>& sign_bytes) const;
  /** Raw device ML-DSA-65 private key for libp2p Host identity binding. */
  Roe<ByteVector> GetDeviceMlDsaPrivateKey() const;
  Roe<ByteVector> GetDeviceMlDsaPublicKey() const;
  /** Account ML-KEM-768 secret (M015). Mints only if identity has no valid KEM yet. */
  Roe<ByteVector> GetOrCreateHybridKemPrivateKey() const;
  Roe<std::string> GetHybridKemPublicKeyB64() const;
  /** This device's own ML-KEM-768 secret (M021): opens the sessions peers start with this device. */
  Roe<ByteVector> GetDeviceKemPrivateKey() const;
  Roe<std::string> GetAccountId() const override;
  Roe<int64_t> InitiationFloor() const override;
  void Flush();

private:
  Roe<void> EnsureLoaded() const;
  Roe<void> Save() const;
  /** Seals and writes `identity` (not necessarily identity_): Update / Modify commit to memory only after it. */
  Roe<void> SaveIdentity(const LocalIdentity& identity) const;
  Roe<void> RequireDek() const;
  std::string StorePath() const;
  std::string ProfileId() const;

  std::string data_dir_;
  std::string profile_id_;
  mutable ByteVector dek_;
  mutable ByteVector identity_seed_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable LocalIdentity identity_;
  mutable std::vector<uint8_t> private_key_;
  mutable bool dirty_ = false;
};

} // namespace pbr
