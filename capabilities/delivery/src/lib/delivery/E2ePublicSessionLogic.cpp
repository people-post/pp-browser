#include "lib/delivery/E2ePublicSessionLogic.h"

#include "lib/delivery/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/SessionKeyDeriver.h"

#include <sodium.h>

#include <array>

namespace pbr {

Roe<E2ePublicPskEnsureResult> EnsureE2ePublicMasterPsk(IPeerSessionStore& psk_store, const PeerSessionKey& target_key,
                                                       const uint32_t session_epoch,
                                                       const ByteVector& peer_kem_public) {
  if (peer_kem_public.empty()) {
    return Error("Peer KEM public key required");
  }
  return EnsureE2ePublicMasterPsk(psk_store, target_key, session_epoch,
                                  [&peer_kem_public]() -> Roe<ByteVector> { return peer_kem_public; });
}

Roe<E2ePublicPskEnsureResult> EnsureE2ePublicMasterPsk(IPeerSessionStore& psk_store, const PeerSessionKey& target_key,
                                                       const uint32_t session_epoch,
                                                       const std::function<Roe<ByteVector>()>& peer_kem_public) {
  if (target_key.channel != CryptoChannel::E2ePublic) {
    return Error("EnsureE2ePublicMasterPsk requires e2e_public");
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
      out.key_init_b64 = (*loaded)->auto_key_init_pending; // not confirmed yet: keep attaching it
    }
    return out;
  }

  auto peer_public = peer_kem_public();
  if (!peer_public) {
    return peer_public.error();
  }
  if (peer_public->empty()) {
    return Error("Peer KEM public key required");
  }
  auto established = AutoKeyEstablishment::EncapsulateForRecipient(*peer_public);
  if (!established) {
    return established.error();
  }
  PeerSessionRecord record;
  if (loaded->has_value()) {
    record = **loaded; // keep what the row holds besides the session (retired epochs, scope …)
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

Roe<E2ePublicSendSession> ResolveE2ePublicSendSession(IPeerSessionStore& psk_store, const PeerSessionKey& target_key,
                                                      const std::function<Roe<ByteVector>()>& peer_kem_public) {
  auto record = psk_store.Load(target_key);
  if (!record) {
    return record.error();
  }
  E2ePublicSendSession session;
  session.epoch = record->has_value() && (*record)->session_epoch > 0 ? (*record)->session_epoch : 1;
  auto ensured = EnsureE2ePublicMasterPsk(psk_store, target_key, session.epoch, peer_kem_public);
  if (!ensured) {
    return ensured.error();
  }
  session.master_psk = std::move(ensured->master_psk);
  session.key_init_b64 = std::move(ensured->key_init_b64);
  return session;
}

std::string AutoKeySessionId(const std::string& key_init_b64) {
  std::array<unsigned char, crypto_generichash_BYTES> hash{};
  crypto_generichash(hash.data(), hash.size(), reinterpret_cast<const unsigned char*>(key_init_b64.data()),
                     key_init_b64.size(), nullptr, 0);
  return BytesToHex(ByteVector(hash.begin(), hash.end()));
}

Roe<void> ReconcileE2ePublicSession(IPeerSessionStore& psk_store, const PeerSessionKey& target_key,
                                    const uint32_t session_epoch, const AutoKeyRead& read,
                                    const std::optional<std::string>& envelope_key_init_b64,
                                    const int64_t envelope_timestamp_ms) {
  auto loaded = psk_store.Load(target_key);
  if (!loaded) {
    return loaded.error();
  }
  const bool have_row = loaded->has_value();
  PeerSessionRecord record;
  if (have_row) {
    record = **loaded;
  } else {
    record.key = target_key;
    record.session_epoch = session_epoch;
  }
  if (have_row && record.master_psk_b64 && record.session_epoch > session_epoch) {
    return {}; // an earlier epoch (retired): not this rule's to change
  }
  if (have_row && record.master_psk_b64 && record.session_epoch < session_epoch) {
    // The peer started the pair's next epoch with a session it opened (it lost its numbering — a restore
    // or a chat reset — and restarts at seq 1 rather than rewinding, D014): move there, keeping the
    // previous session for messages still on their way under it.
    if (!read.via_key_init || !envelope_key_init_b64 || envelope_key_init_b64->empty()) {
      return {};
    }
    record.retired_psks.push_back(RetiredPskEntry{
        .epoch = record.session_epoch, .master_psk_b64 = *record.master_psk_b64, .retired_at = envelope_timestamp_ms});
    record.session_epoch = session_epoch;
    record.master_psk_b64 = Base64Encode(read.master_psk);
    record.auto_key_session_id = AutoKeySessionId(*envelope_key_init_b64);
    record.auto_key_init_pending.reset();
    record.auto_key_peer_heard_ms = envelope_timestamp_ms;
    return psk_store.Save(record);
  }
  // The peer was heard under the stored session at this (its own, signed) time.
  const auto note_heard = [&record, envelope_timestamp_ms]() {
    if (!record.auto_key_peer_heard_ms || envelope_timestamp_ms > *record.auto_key_peer_heard_ms) {
      record.auto_key_peer_heard_ms = envelope_timestamp_ms;
      return true;
    }
    return false;
  };

  if (!read.via_key_init) {
    // The peer wrote under our stored session: it has it. A session we opened is confirmed.
    bool changed = note_heard();
    if (record.auto_key_init_pending) {
      record.auto_key_init_pending.reset();
      changed = true;
    }
    return changed ? psk_store.Save(record) : Roe<void>{};
  }

  if (!envelope_key_init_b64 || envelope_key_init_b64->empty()) {
    return {};
  }
  const std::string theirs = AutoKeySessionId(*envelope_key_init_b64);
  if (record.master_psk_b64 && record.auto_key_session_id == theirs) {
    return note_heard() ? psk_store.Save(record) : Roe<void>{}; // already ours
  }
  if (record.master_psk_b64 && record.auto_key_init_pending && record.auto_key_session_id &&
      *record.auto_key_session_id < theirs) {
    return {}; // both opened at once; ours wins the tie — the peer moves to it
  }
  if (record.master_psk_b64 && record.auto_key_peer_heard_ms &&
      envelope_timestamp_ms <= *record.auto_key_peer_heard_ms) {
    // The peer has since written under the stored session: this opener is older than that (late or
    // replayed) — adopting it would roll the pair back to a session the peer has left.
    return {};
  }
  record.session_epoch = session_epoch;
  record.master_psk_b64 = Base64Encode(read.master_psk);
  record.auto_key_session_id = theirs;
  record.auto_key_init_pending.reset(); // the peer opened it: nothing of ours to announce
  record.auto_key_peer_heard_ms = envelope_timestamp_ms;
  return psk_store.Save(record);
}

Roe<void> RestartE2ePublicSession(IPeerSessionStore& psk_store, const PeerSessionKey& target_key,
                                  const uint32_t old_epoch, const uint32_t new_epoch, const int64_t now_ms) {
  auto loaded = psk_store.Load(target_key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value()) {
    return {};
  }
  PeerSessionRecord record = **loaded;
  if (record.master_psk_b64 && old_epoch < new_epoch) {
    record.retired_psks.push_back(
        RetiredPskEntry{.epoch = old_epoch, .master_psk_b64 = *record.master_psk_b64, .retired_at = now_ms});
  }
  record.session_epoch = new_epoch;
  record.master_psk_b64.reset();
  record.psk_fingerprint.reset();
  record.auto_key_session_id.reset();
  record.auto_key_init_pending.reset();
  record.auto_key_peer_heard_ms.reset();
  record.restart_pending = false;
  return psk_store.Save(record);
}

Roe<ByteVector> DeriveE2ePublicSessionKey(const ByteVector& master_psk, const uint32_t session_epoch) {
  return SessionKeyDeriver::Derive(master_psk, CryptoChannel::E2ePublic, session_epoch);
}

} // namespace pbr
