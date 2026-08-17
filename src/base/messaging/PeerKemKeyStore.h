#pragma once

#include "common/Error.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace pbr {

struct PeerKemKeyRecord {
  std::string kem_public_key_b64;
  std::string source;
  std::optional<std::string> source_ref;
};

/** Local cache of **account** KEM public keys, keyed by communicating identity (E024 / M015). */
class PeerKemKeyStore {
public:
  void Put(const std::string& peer_identity_kind, const std::string& peer_identity_value, PeerKemKeyRecord record);
  std::optional<PeerKemKeyRecord> Get(const std::string& peer_identity_kind,
                                      const std::string& peer_identity_value) const;
  void Clear();

private:
  static std::string MakeKey(const std::string& kind, const std::string& value);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, PeerKemKeyRecord> keys_;
};

class IPeerKemKeyResolver {
public:
  virtual ~IPeerKemKeyResolver() = default;
  virtual Roe<PeerKemKeyRecord> Resolve(const std::string& peer_identity_kind,
                                          const std::string& peer_identity_value) = 0;
};

class PeerKemKeyResolver : public IPeerKemKeyResolver {
public:
  explicit PeerKemKeyResolver(PeerKemKeyStore& store);

  Roe<PeerKemKeyRecord> Resolve(const std::string& peer_identity_kind,
                                const std::string& peer_identity_value) override;

private:
  PeerKemKeyStore& store_;
};

} // namespace pbr
