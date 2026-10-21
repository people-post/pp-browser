#include "domain/messaging/E2ePublicSessionLogic.h"

#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SessionKeyDeriver.h"

#include <sodium.h>

#include <array>

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

  auto loaded = psk_store.Load(target_key);
  if (!loaded) {
    return loaded.error();
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
    if (loaded->has_value() && (*loaded)->session_epoch == session_epoch) {
      out.key_init_b64 = (*loaded)->auto_key_init_pending;  // not confirmed yet: keep attaching it
    }
    return out;
  }

  auto established = AutoKeyEstablishment::EncapsulateForRecipient(peer_kem_public);
  if (!established) {
    return established.error();
  }
  PskSessionRecord record;
  if (loaded->has_value()) {
    record = **loaded;  // keep what the row holds besides the session (retired epochs, scope …)
  }
  record.key = target_key;
  record.session_epoch = session_epoch;
  record.master_psk_b64 = Base64Encode(established->master_psk);
  record.auto_key_session_id = AutoKeySessionId(established->key_init_b64);
  record.auto_key_init_pending = established->key_init_b64;
  if (auto saved = psk_store.Save(record); !saved) {
    return saved.error();
  }
  E2ePublicPskEnsureResult out;
  out.master_psk = std::move(established->master_psk);
  out.key_init_b64 = std::move(established->key_init_b64);
  out.created = true;
  return out;
}

std::string AutoKeySessionId(const std::string& key_init_b64) {
  std::array<unsigned char, crypto_generichash_BYTES> hash{};
  crypto_generichash(hash.data(), hash.size(), reinterpret_cast<const unsigned char*>(key_init_b64.data()),
                     key_init_b64.size(), nullptr, 0);
  return BytesToHex(ByteVector(hash.begin(), hash.end()));
}

Roe<void> ReconcileE2ePublicSession(IPskSessionStore& psk_store, const ChatTargetKey& target_key,
                                    const uint32_t session_epoch, const AutoKeyRead& read,
                                    const std::optional<std::string>& envelope_key_init_b64) {
  auto loaded = psk_store.Load(target_key);
  if (!loaded) {
    return loaded.error();
  }
  const bool have_row = loaded->has_value();
  PskSessionRecord record;
  if (have_row) {
    record = **loaded;
  } else {
    record.key = target_key;
    record.session_epoch = session_epoch;
  }
  if (have_row && record.master_psk_b64 && record.session_epoch != session_epoch) {
    return {};  // another epoch (a retired one, or one rotation owns): not this rule's to change
  }

  if (!read.via_key_init) {
    // The peer wrote under our stored session: it has it. A session we opened is confirmed.
    if (record.auto_key_init_pending) {
      record.auto_key_init_pending.reset();
      return psk_store.Save(record);
    }
    return {};
  }

  if (!envelope_key_init_b64 || envelope_key_init_b64->empty()) {
    return {};
  }
  const std::string theirs = AutoKeySessionId(*envelope_key_init_b64);
  if (record.master_psk_b64 && record.auto_key_session_id == theirs) {
    return {};  // already ours
  }
  if (record.master_psk_b64 && record.auto_key_init_pending && record.auto_key_session_id &&
      *record.auto_key_session_id < theirs) {
    return {};  // both opened at once; ours wins the tie — the peer moves to it
  }
  record.session_epoch = session_epoch;
  record.master_psk_b64 = Base64Encode(read.master_psk);
  record.auto_key_session_id = theirs;
  record.auto_key_init_pending.reset();  // the peer opened it: nothing of ours to announce
  return psk_store.Save(record);
}

Roe<ByteVector> DeriveE2ePublicSessionKey(const ByteVector& master_psk, const uint32_t session_epoch) {
  return SessionKeyDeriver::Derive(master_psk, CryptoChannel::E2ePublic, session_epoch);
}

} // namespace pbr
