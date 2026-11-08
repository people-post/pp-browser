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

Roe<EnsuredE2ePublicSessionKey> E2ePublicSessionKeys::Ensure(const std::string& peer_identity) {
  auto session = Resolve(peer_identity);
  if (!session) {
    return session.error();
  }
  auto session_key = DeriveE2ePublicSessionKey(session->master_psk, session->epoch);
  if (!session_key) {
    return session_key.error();
  }
  EnsuredE2ePublicSessionKey out;
  out.session_key = std::move(*session_key);
  if (session->key_init_b64 && !session->key_init_b64->empty()) {
    out.first_message_key_init_b64 = std::move(*session->key_init_b64);
  }
  return out;
}

Roe<ByteVector> E2ePublicSessionKeys::PeerKemPublicKey(const PeerSessionKey& key) const {
  auto kem = kem_key_resolver_.Resolve(key.peer_identity_kind, key.peer_identity_value);
  if (!kem) {
    return kem.error();
  }
  return Base64Decode(kem->kem_public_key_b64);
}

} // namespace pbr
