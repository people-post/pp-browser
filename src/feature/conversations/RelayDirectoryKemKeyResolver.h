#pragma once

#include "domain/messaging/PeerKemKeyStore.h"
#include "domain/net/OrgBackendClients.h"
#include "common/PbrCompat.h"

namespace pbr {

class RelayDirectoryKemKeyResolver : public IPeerKemKeyResolver {
public:
  RelayDirectoryKemKeyResolver(PeerKemKeyStore& store, IDirectoryClient& directory);

  Roe<PeerKemKeyRecord> Resolve(const std::string& peer_identity_kind,
                                const std::string& peer_identity_value) override;

private:
  static bool IsAccountKind(const std::string& peer_identity_kind);

  PeerKemKeyStore& store_;
  IDirectoryClient& directory_;
};

} // namespace pbr
