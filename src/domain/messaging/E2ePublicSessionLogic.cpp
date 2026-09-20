#include "domain/messaging/E2ePublicSessionLogic.h"

#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SessionKeyDeriver.h"

namespace pbr {

Roe<E2ePublicPskEnsureResult> EnsureE2ePublicMasterPsk(IPskSessionStore& psk_store,
                                                      const ChatTargetKey& target_key,
                                                      const uint32_t session_epoch,
                                                      const ByteVector& peer_kem_public) {
  if (target_key.channel != CryptoChannel::E2ePublic) {
    return Error("EnsureE2ePublicMasterPsk requires e2e_public");
  }
  if (peer_kem_public.empty()) {
    return Error("Peer KEM public key required");
  }

  auto existing = psk_store.ResolveMasterPskForEpoch(target_key, session_epoch);
  if (!existing) {
    return existing.error();
  }
  if (existing->has_value()) {
    auto decoded = Base64Decode(**existing);
    if (!decoded) {
      return decoded.error();
    }
    E2ePublicPskEnsureResult out;
    out.master_psk = std::move(*decoded);
    out.created = false;
    return out;
  }

  auto established = AutoKeyEstablishment::EncapsulateForRecipient(peer_kem_public);
  if (!established) {
    return established.error();
  }
  PskSessionRecord record;
  record.key = target_key;
  record.session_epoch = session_epoch;
  record.master_psk_b64 = Base64Encode(established->master_psk);
  if (auto saved = psk_store.Save(record); !saved) {
    return saved.error();
  }
  E2ePublicPskEnsureResult out;
  out.master_psk = std::move(established->master_psk);
  out.key_init_b64 = std::move(established->key_init_b64);
  out.created = true;
  return out;
}

Roe<ByteVector> DeriveE2ePublicSessionKey(const ByteVector& master_psk, const uint32_t session_epoch) {
  return SessionKeyDeriver::Derive(master_psk, CryptoChannel::E2ePublic, session_epoch);
}

} // namespace pbr
