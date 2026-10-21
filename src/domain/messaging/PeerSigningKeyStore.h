#pragma once

#include "common/Error.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

struct PeerSigningKeyRecord {
  std::string signing_public_key_b64;
  std::string source;
  std::optional<std::string> source_ref;
};

/** Local ML-DSA verify key cache keyed by communicating identity (E016). */
class PeerSigningKeyStore {
public:
  /**
   * Keep `record` as the signing key of an account. An Account ID is self-certifying
   * (BLAKE2b-256 of the account ML-DSA key, SERVICE_ENDPOINTS.md): a key that does not hash to the
   * account it is filed under — or one filed under any other identity kind — is refused, whoever
   * offers it (directory, contact card, sync). False when refused.
   */
  bool Put(const std::string& peer_identity_kind, const std::string& peer_identity_value,
           PeerSigningKeyRecord record);
  std::optional<PeerSigningKeyRecord> Get(const std::string& peer_identity_kind,
                                          const std::string& peer_identity_value) const;
  /** Tests whose fixtures use readable ids (`account:peer`) instead of derived ones: no binding check. */
  void PutUncheckedForTest(const std::string& peer_identity_kind, const std::string& peer_identity_value,
                           PeerSigningKeyRecord record);
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
