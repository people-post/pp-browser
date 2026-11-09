#pragma once

#include "foundation/crypto/CryptoUtil.h"
#include "common/directory/DirectoryJson.h"
#include "lib/delivery/E2ePublicSessionLogic.h"
#include "lib/delivery/IPeerSessionStore.h"
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"

#include <string>

namespace pbr::test {

/**
 * What delivery's `ensure_peer_session_key` answers for a session the test seeded in `psk` (tests have
 * no KEM directory to open one): the e2e_public session key at the stored epoch.
 */
inline Roe<ByteVector> SeededSessionKey(const IPeerSessionStore& psk, const std::string& peer) {
  PeerSessionKey key;
  key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  key.peer_identity_value = peer;
  key.channel = CryptoChannel::E2ePublic;
  auto record = psk.Load(key);
  if (!record) {
    return record.error();
  }
  if (!record->has_value()) {
    return Error("No session seeded for the peer");
  }
  const uint32_t epoch = (*record)->session_epoch > 0 ? (*record)->session_epoch : 1;
  auto master_b64 = psk.ResolveMasterPskForEpoch(key, epoch);
  if (!master_b64) {
    return master_b64.error();
  }
  if (!master_b64->has_value()) {
    return Error("No master PSK seeded for the peer");
  }
  auto master = Base64Decode(**master_b64);
  if (!master) {
    return master.error();
  }
  return DeriveE2ePublicSessionKey(*master, epoch);
}

} // namespace pbr::test
