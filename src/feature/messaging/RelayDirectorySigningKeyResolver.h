#pragma once

#include "base/messaging/PeerSigningKeyStore.h"
#include "base/net/ServiceClients.h"
#include "base/people/ContactTypes.h"

#include "common/Error.h"

namespace pbr {

/** E016 — cache-first resolver with lazy directory lookup. */
class RelayDirectorySigningKeyResolver : public IPeerSigningKeyResolver {
public:
  RelayDirectorySigningKeyResolver(PeerSigningKeyStore& store, IDirectoryClient& directory);

  Roe<PeerSigningKeyRecord> Resolve(const std::string& peer_identity_kind,
                                    const std::string& peer_identity_value) override;

private:
  static bool IsRelayUserKind(const std::string& peer_identity_kind);

  PeerSigningKeyStore& store_;
  IDirectoryClient& directory_;
};

} // namespace pbr
