#include "domain/delivery/E2ePublicSessionKeys.h"

#include "common/Utilities.h"
#include "common/directory/DirectoryJson.h"
#include "foundation/crypto/CryptoUtil.h"

namespace pbr {

E2ePublicSessionKeys::E2ePublicSessionKeys(IPskSessionStore& psk_store, IPeerKemKeyResolver& kem_key_resolver)
    : psk_store_(psk_store), kem_key_resolver_(kem_key_resolver) {}

Roe<E2ePublicSessionKeys::Session> E2ePublicSessionKeys::Resolve(const std::string& peer_identity) {
  if (peer_identity.empty()) {
    return Error("Peer identity required");
  }
  Session session;
  session.target_key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  session.target_key.peer_identity_value = peer_identity;
  session.target_key.channel = CryptoChannel::E2ePublic;
  // The pair's session is the account pair's, not a thread's: its active epoch is the stored one.
  auto record = psk_store_.Load(session.target_key);
  if (!record) {
    return record.error();
  }
  session.epoch = record->has_value() && (*record)->session_epoch > 0 ? (*record)->session_epoch : 1;
  auto peer_public = PeerKemPublicKey(session.target_key);
  if (!peer_public) {
    return peer_public.error();
  }
  auto ensured = EnsureE2ePublicMasterPsk(psk_store_, session.target_key, session.epoch, *peer_public);
  if (!ensured) {
    return ensured.error();
  }
  session.master_psk = std::move(ensured->master_psk);
  session.key_init_b64 = std::move(ensured->key_init_b64);
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

Roe<ByteVector> E2ePublicSessionKeys::PeerKemPublicKey(const ChatTargetKey& key) const {
  auto kem = kem_key_resolver_.Resolve(key.peer_identity_kind, key.peer_identity_value);
  if (!kem) {
    return kem.error();
  }
  return Base64Decode(kem->kem_public_key_b64);
}

} // namespace pbr
