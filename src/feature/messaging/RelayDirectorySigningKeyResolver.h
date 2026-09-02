#pragma once

#include "base/messaging/PeerSigningKeyStore.h"
#include "domain/net/ServiceClients.h"
#include "domain/people/ContactTypes.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

/** E016 — cache-first resolver with lazy directory lookup. */
class RelayDirectorySigningKeyResolver : public IPeerSigningKeyResolver {
public:
  RelayDirectorySigningKeyResolver(PeerSigningKeyStore& store, IDirectoryClient& directory);

  Roe<PeerSigningKeyRecord> Resolve(const std::string& peer_identity_kind,
                                    const std::string& peer_identity_value) override;

private:
  static bool IsAccountKind(const std::string& peer_identity_kind);

  PeerSigningKeyStore& store_;
  IDirectoryClient& directory_;
};

} // namespace pbr
