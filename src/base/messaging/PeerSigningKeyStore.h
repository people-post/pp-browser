#pragma once

#include "common/Error.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace pbr {

struct PeerSigningKeyRecord {
  std::string signing_public_key_b64;
  std::string source;
  std::optional<std::string> source_ref;
};

/** Local Ed25519 verify key cache keyed by communicating identity (E016). */
class PeerSigningKeyStore {
public:
  void Put(const std::string& peer_identity_kind, const std::string& peer_identity_value,
           PeerSigningKeyRecord record);
  std::optional<PeerSigningKeyRecord> Get(const std::string& peer_identity_kind,
                                          const std::string& peer_identity_value) const;
  void Clear();

private:
  static std::string MakeKey(const std::string& kind, const std::string& value);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, PeerSigningKeyRecord> keys_;
};

class IPeerSigningKeyResolver {
public:
  virtual ~IPeerSigningKeyResolver() = default;
  virtual Roe<PeerSigningKeyRecord> Resolve(const std::string& peer_identity_kind,
                                            const std::string& peer_identity_value) = 0;
};

class PeerSigningKeyResolver : public IPeerSigningKeyResolver {
public:
  explicit PeerSigningKeyResolver(PeerSigningKeyStore& store);

  Roe<PeerSigningKeyRecord> Resolve(const std::string& peer_identity_kind,
                                    const std::string& peer_identity_value) override;

private:
  PeerSigningKeyStore& store_;
};

} // namespace pbr
