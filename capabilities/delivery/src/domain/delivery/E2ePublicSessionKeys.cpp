#include "domain/delivery/E2ePublicSessionKeys.h"

#include "common/Utilities.h"
#include "common/directory/DirectoryJson.h"
#include "foundation/crypto/CryptoUtil.h"

namespace pbr {

E2ePublicSessionKeys::E2ePublicSessionKeys(IPeerSessionStore& psk_store, IPeerKemKeyResolver& kem_key_resolver)
    : psk_store_(psk_store), kem_key_resolver_(kem_key_resolver) {}

Roe<E2ePublicSessionKeys::Session> E2ePublicSessionKeys::Resolve(const std::string& peer_identity) {
  if (peer_identity.empty()) {
    return Error("Peer identity required");
  }
  Session session;
  session.target_key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  session.target_key.peer_identity_value = peer_identity;
  session.target_key.channel = CryptoChannel::E2ePublic;
  // The account pair's session, at its stored epoch; the peer's KEM key (maybe a directory lookup) only to open it.
  auto resolved = ResolveE2ePublicSendSession(psk_store_, session.target_key,
                                              [this, &session]() { return PeerKemPublicKey(session.target_key); });
  if (!resolved) {
    return resolved.error();
  }
  session.epoch = resolved->epoch;
  session.master_psk = std::move(resolved->master_psk);
  session.key_init_b64 = std::move(resolved->key_init_b64);
  return session;
}

Roe<E2ePublicSessionKeys::Session> E2ePublicSessionKeys::ResolveDevice(const std::string& peer_identity,
                                                                       const AccountCardDevice& device) {
  if (peer_identity.empty() || device.peer_id.empty()) {
    return Error("Peer device required");
  }
  Session session;
  session.target_key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  session.target_key.peer_identity_value = peer_identity;
  session.target_key.channel = CryptoChannel::E2ePublic;
  session.target_key.peer_device = device.peer_id;
  // The device pair's session (M021), shared with the chat stream; opened to the device's own KEM key.
  auto resolved = ResolveE2ePublicSendSession(psk_store_, session.target_key,
                                              [&device]() { return Base64Decode(device.kem_public_key_b64); });
  if (!resolved) {
    return resolved.error();
  }
  session.epoch = resolved->epoch;
  session.master_psk = std::move(resolved->master_psk);
  session.key_init_b64 = std::move(resolved->key_init_b64);
  return session;
}

Roe<ByteVector> E2ePublicSessionKeys::Ensure(const std::string& peer_identity) {
  auto session = Resolve(peer_identity);
  if (!session) {
    return session.error();
  }
  return DeriveE2ePublicSessionKey(session->master_psk, session->epoch);
}

Roe<ByteVector> E2ePublicSessionKeys::Held(const std::string& peer_identity) const {
  if (peer_identity.empty()) {
    return Error("Peer identity required");
  }
  PeerSessionKey key;
  key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  key.peer_identity_value = peer_identity;
  key.channel = CryptoChannel::E2ePublic;
  auto record = psk_store_.Load(key);
  if (!record) {
    return record.error();
  }
  if (!record->has_value()) {
    return Error("No session held with the peer");
  }
  const uint32_t epoch = (*record)->session_epoch > 0 ? (*record)->session_epoch : 1;
  auto master_b64 = psk_store_.ResolveMasterPskForEpoch(key, epoch);
  if (!master_b64) {
    return master_b64.error();
  }
  if (!master_b64->has_value()) {
    return Error("No session held with the peer");
  }
  auto master = Base64Decode(**master_b64);
  if (!master) {
    return master.error();
  }
  return DeriveE2ePublicSessionKey(*master, epoch);
}

Roe<ByteVector> E2ePublicSessionKeys::PeerKemPublicKey(const PeerSessionKey& key) const {
  auto kem = kem_key_resolver_.Resolve(key.peer_identity_kind, key.peer_identity_value);
  if (!kem) {
    return kem.error();
  }
  return Base64Decode(kem->kem_public_key_b64);
}

} // namespace pbr
