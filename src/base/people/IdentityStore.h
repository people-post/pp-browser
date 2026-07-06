#pragma once

#include <cstdint>
#include "common/Error.h"
#include "common/Module.h"
#include "base/crypto/CryptoTypes.h"
#include "base/people/IdentityTypes.h"

#include <mutex>
#include <string>
#include <vector>
#include <vector>

namespace pbr {

class IdentityStore : public Module {
public:
  explicit IdentityStore(std::string data_dir);

  Roe<LocalIdentity> LoadOrCreate();
  Roe<LocalIdentity> Get() const;
  Roe<LocalIdentity> Update(const LocalIdentity& identity);
  Roe<std::string> SignPayload(const std::string& canonical_json) const;
  Roe<std::string> SignBytes(const std::vector<uint8_t>& sign_bytes) const;
  Roe<ByteVector> GetOrCreateHybridKemPrivateKey() const;
  Roe<std::string> GetHybridKemPublicKeyB64() const;
  void Flush();

private:
  Roe<void> EnsureLoaded() const;
  Roe<void> Save() const;
  std::string StorePath() const;

  std::string data_dir_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable LocalIdentity identity_;
  mutable std::vector<uint8_t> private_key_;
  mutable bool dirty_ = false;
};

} // namespace pbr
