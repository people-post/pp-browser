#pragma once

#include "base/messaging/PeerKemKeyStore.h"
#include "base/net/ServiceClients.h"

namespace pbr {

class RelayDirectoryKemKeyResolver : public IPeerKemKeyResolver {
public:
  RelayDirectoryKemKeyResolver(PeerKemKeyStore& store, IDirectoryClient& directory);

  Roe<PeerKemKeyRecord> Resolve(const std::string& peer_identity_kind,
                                const std::string& peer_identity_value) override;

private:
  static bool IsRelayUserKind(const std::string& peer_identity_kind);

  PeerKemKeyStore& store_;
  IDirectoryClient& directory_;
};

} // namespace pbr
