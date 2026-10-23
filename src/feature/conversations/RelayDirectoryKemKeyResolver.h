#pragma once

#include "lib/messaging/PeerKemKeyStore.h"
#include "lib/net/OrgBackendClients.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class RelayDirectoryKemKeyResolver : public IPeerKemKeyResolver {
public:
  RelayDirectoryKemKeyResolver(PeerKemKeyStore& store, mesh::IDirectoryClient& directory);

  Roe<PeerKemKeyRecord> Resolve(const std::string& peer_identity_kind,
                                const std::string& peer_identity_value) override;

private:
  static bool IsAccountKind(const std::string& peer_identity_kind);

  PeerKemKeyStore& store_;
  mesh::IDirectoryClient& directory_;
};

} // namespace pbr
