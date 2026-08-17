#include "feature/messaging/PublicPskLockCoordinator.h"

#include "base/crypto/AutoKeyEstablishment.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/HybridKem.h"
#include "base/crypto/PskBundleCodec.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/ThreadTypes.h"
#include "common/Utilities.h"

namespace pbr {

PublicPskLockCoordinator::PublicPskLockCoordinator(IThreadStore& store, IPskSessionStore& psk_store)
    : store_(store), psk_store_(psk_store) {}

Roe<ChatTargetKey> PublicPskLockCoordinator::TargetKeyForPublicThread(const std::string& thread_id) const {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind != ThreadKind::Direct || (*thread)->channel != ThreadChannel::E2ePublic) {
    return Error("Device-lock applies to public 1:1 chats only");
  }
  return E2eRelayPayloadCodec::ChatTargetFromThread(**thread);
}

Roe<PskSessionRecord> PublicPskLockCoordinator::LoadRequired(const ChatTargetKey& key) const {
  auto loaded = psk_store_.Load(key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value() || !loaded->value().master_psk_b64) {
    return Error("Public chat has no encryption key yet");
  }
  return loaded->value();
}

Roe<PublicKeyScope> PublicPskLockCoordinator::GetKeyScope(const std::string& thread_id) const {
  auto key = TargetKeyForPublicThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto loaded = psk_store_.Load(*key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value()) {
    return PublicKeyScope::Account;
  }
  return loaded->value().key_scope;
}

Roe<bool> PublicPskLockCoordinator::CanLockToThisDevice(const std::string& thread_id) const {
  auto key = TargetKeyForPublicThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto loaded = psk_store_.Load(*key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value() || !loaded->value().master_psk_b64) {
    return false;
  }
  const PublicKeyScope scope = loaded->value().key_scope;
  return scope == PublicKeyScope::Account;
}

Roe<bool> PublicPskLockCoordinator::ShouldAutoRekey(const std::string& thread_id, const int64_t now_ms) const {
  auto key = TargetKeyForPublicThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto loaded = psk_store_.Load(*key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value()) {
    return false;
  }
  const PskSessionRecord& record = loaded->value();
  if (record.key_scope != PublicKeyScope::DevicePair || !record.peer_thread_kem_pk_b64) {
    return false;
  }
  if (record.psk_rotate_msg_count >= kPublicPskAutoRotateMsgCount) {
    return true;
  }
  if (record.last_psk_rotate_at && now_ms - *record.last_psk_rotate_at >= kPublicPskAutoRotateIntervalMs) {
    return true;
  }
  return false;
}

Roe<void> PublicPskLockCoordinator::NoteTraffic(const std::string& thread_id) {
  auto key = TargetKeyForPublicThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto loaded = psk_store_.Load(*key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value() || loaded->value().key_scope != PublicKeyScope::DevicePair) {
    return {};
  }
  PskSessionRecord record = loaded->value();
  ++record.psk_rotate_msg_count;
  return psk_store_.Save(record);
}

Roe<void> PublicPskLockCoordinator::EnsureConversationKem(PskSessionRecord& record) {
  if (record.thread_kem_pk_b64 && record.thread_kem_sk_b64 && !record.thread_kem_pk_b64->empty() &&
      !record.thread_kem_sk_b64->empty()) {
    return {};
  }
  auto pair = HybridKem::GenerateKeyPair();
  if (!pair) {
    return pair.error();
  }
  record.thread_kem_pk_b64 = Base64Encode(pair->public_key);
  record.thread_kem_sk_b64 = Base64Encode(pair->private_key);
  return {};
}

Roe<PublicPskRotatePlan> PublicPskLockCoordinator::PrepareRotate(const std::string& thread_id,
                                                                const PublicPskRotateKind kind,
                                                                const ByteVector& peer_account_kem_pk,
                                                                const int64_t now_ms) {
  (void)now_ms;
  auto key = TargetKeyForPublicThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto record = LoadRequired(*key);
  if (!record) {
    return record.error();
  }
  if (record->key_scope == PublicKeyScope::LockedOut) {
    return Error("This chat continues on another device");
  }
  if (kind == PublicPskRotateKind::Lock && record->key_scope != PublicKeyScope::Account) {
    return Error("This chat is already only on this device");
  }
  if (kind == PublicPskRotateKind::Auto && record->key_scope != PublicKeyScope::DevicePair) {
    return Error("Auto-rekey requires both sides on this-device mode");
  }
  if (auto kem = EnsureConversationKem(*record); !kem) {
    return kem.error();
  }

  ByteVector wrap_pk;
  std::string wrap_kind = kPskRotateWrapAccountKem;
  if (record->peer_thread_kem_pk_b64 && !record->peer_thread_kem_pk_b64->empty()) {
    auto decoded = Base64Decode(*record->peer_thread_kem_pk_b64);
    if (!decoded) {
      return decoded.error();
    }
    wrap_pk = std::move(*decoded);
    wrap_kind = kPskRotateWrapThreadKem;
  } else if (kind == PublicPskRotateKind::Auto) {
    return Error("Auto-rekey needs the peer conversation key");
  } else {
    if (peer_account_kem_pk.size() != kHybridKemPublicKeyBytes) {
      return Error("Invalid peer account KEM public key");
    }
    wrap_pk = peer_account_kem_pk;
  }

  auto old_psk = Base64Decode(*record->master_psk_b64);
  if (!old_psk) {
    return old_psk.error();
  }
  auto established = AutoKeyEstablishment::EncapsulateForRecipient(wrap_pk);
  if (!established) {
    return established.error();
  }
  auto key_init_hash = AutoKeyEstablishment::HashKeyInitB64(established->key_init_b64);
  if (!key_init_hash) {
    return key_init_hash.error();
  }

  PublicPskRotatePlan plan;
  plan.key = *key;
  plan.thread_id = thread_id;
  plan.old_epoch = record->session_epoch;
  plan.old_master_psk = std::move(*old_psk);
  plan.new_master_psk = established->master_psk;
  plan.key_init_b64 = established->key_init_b64;
  plan.detail.rotation_id = util::GenerateUuid();
  plan.detail.new_epoch = record->session_epoch + 1;
  plan.detail.wrap_kind = wrap_kind;
  plan.detail.thread_kem_pk_b64 = *record->thread_kem_pk_b64;
  plan.detail.key_init_hash = *key_init_hash;
  plan.kind = kind;
  if (kind == PublicPskRotateKind::Auto) {
    plan.next_scope = PublicKeyScope::DevicePair;
  } else if (record->peer_thread_kem_pk_b64 && !record->peer_thread_kem_pk_b64->empty()) {
    plan.next_scope = PublicKeyScope::DevicePair;
  } else {
    plan.next_scope = PublicKeyScope::DeviceSelf;
  }

  record->last_rotation_id = plan.detail.rotation_id;
  if (auto saved = psk_store_.Save(*record); !saved) {
    return saved.error();
  }
  return plan;
}

Roe<PublicPskRotatePlan> PublicPskLockCoordinator::PrepareLock(const std::string& thread_id,
                                                              const ByteVector& peer_account_kem_pk,
                                                              const int64_t now_ms) {
  return PrepareRotate(thread_id, PublicPskRotateKind::Lock, peer_account_kem_pk, now_ms);
}

Roe<PublicPskRotatePlan> PublicPskLockCoordinator::PrepareAutoRekey(const std::string& thread_id,
                                                                   const int64_t now_ms) {
  return PrepareRotate(thread_id, PublicPskRotateKind::Auto, {}, now_ms);
}

Roe<void> PublicPskLockCoordinator::Commit(const PublicPskRotatePlan& plan, const int64_t now_ms) {
  auto loaded = LoadRequired(plan.key);
  if (!loaded) {
    return loaded.error();
  }
  if (loaded->last_rotation_id && *loaded->last_rotation_id != plan.detail.rotation_id) {
    return {};
  }
  if (auto cancelled = store_.CancelOldEpochPending(plan.thread_id, plan.old_epoch); !cancelled) {
    return cancelled.error();
  }
  PskSessionRecord record = *loaded;
  RetiredPskEntry retired;
  retired.epoch = plan.old_epoch;
  retired.master_psk_b64 = Base64Encode(plan.old_master_psk);
  retired.retired_at = now_ms;
  record.retired_psks.push_back(std::move(retired));
  PskBundleCodec::CapRetiredTail(record.retired_psks, plan.detail.new_epoch);
  record.master_psk_b64 = Base64Encode(plan.new_master_psk);
  record.session_epoch = plan.detail.new_epoch;
  record.key_scope = plan.next_scope;
  record.last_psk_rotate_at = now_ms;
  record.psk_rotate_msg_count = 0;
  record.last_rotation_id = plan.detail.rotation_id;
  if (auto saved = psk_store_.Save(record); !saved) {
    return saved.error();
  }
  return store_.AdoptChatTargetEpoch(plan.thread_id, plan.detail.new_epoch);
}

Roe<void> PublicPskLockCoordinator::AbortPrepare(const PublicPskRotatePlan& plan) {
  auto loaded = psk_store_.Load(plan.key);
  if (!loaded || !loaded->has_value()) {
    return {};
  }
  PskSessionRecord record = loaded->value();
  if (record.last_rotation_id && *record.last_rotation_id == plan.detail.rotation_id) {
    record.last_rotation_id.reset();
    return psk_store_.Save(record);
  }
  return {};
}

Roe<PublicKeyScope> PublicPskLockCoordinator::ApplyInbound(const std::string& thread_id,
                                                           const RelayEnvelope& envelope,
                                                           const ThreadMessage& message,
                                                           const ByteVector& local_account_kem_sk,
                                                           const std::string& local_account_id,
                                                           const int64_t now_ms) {
  auto key = TargetKeyForPublicThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto detail = PskRotateCodec::Decode(message);
  if (!detail) {
    return detail.error();
  }
  if (!envelope.body.e2e.key_init_b64 || envelope.body.e2e.key_init_b64->empty()) {
    return Error("psk_rotate missing key_init_b64");
  }
  auto hash = AutoKeyEstablishment::HashKeyInitB64(*envelope.body.e2e.key_init_b64);
  if (!hash) {
    return hash.error();
  }
  if (*hash != detail->key_init_hash) {
    return Error("psk_rotate key_init_hash mismatch");
  }

  auto record = LoadRequired(*key);
  if (!record) {
    return record.error();
  }
  if (record->peer_thread_kem_pk_b64 != detail->thread_kem_pk_b64) {
    record->peer_thread_kem_pk_b64 = detail->thread_kem_pk_b64;
  }

  const bool local_wins =
      record->last_rotation_id && PskRotateCodec::RotationIdWins(*record->last_rotation_id, detail->rotation_id);
  if (local_wins) {
    if (auto saved = psk_store_.Save(*record); !saved) {
      return saved.error();
    }
    return record->key_scope;
  }

  // Initiator siblings share the sender account id but not the new PSK (wrap is to the peer).
  if (!local_account_id.empty() && envelope.sender_contact_id == local_account_id) {
    record->key_scope = PublicKeyScope::LockedOut;
    if (auto saved = psk_store_.Save(*record); !saved) {
      return saved.error();
    }
    return PublicKeyScope::LockedOut;
  }

  ByteVector wrap_sk;
  if (detail->wrap_kind == kPskRotateWrapThreadKem) {
    if (!record->thread_kem_sk_b64) {
      record->key_scope = PublicKeyScope::LockedOut;
      (void)psk_store_.Save(*record);
      return PublicKeyScope::LockedOut;
    }
    auto decoded = Base64Decode(*record->thread_kem_sk_b64);
    if (!decoded) {
      return decoded.error();
    }
    wrap_sk = std::move(*decoded);
  } else {
    wrap_sk = local_account_kem_sk;
  }

  auto new_psk = AutoKeyEstablishment::DeriveMasterPskFromKeyInit(wrap_sk, *envelope.body.e2e.key_init_b64);
  if (!new_psk) {
    record->key_scope = PublicKeyScope::LockedOut;
    if (auto saved = psk_store_.Save(*record); !saved) {
      return saved.error();
    }
    return PublicKeyScope::LockedOut;
  }

  if (auto cancelled = store_.CancelOldEpochPending(thread_id, record->session_epoch); !cancelled) {
    return cancelled.error();
  }
  RetiredPskEntry retired;
  retired.epoch = record->session_epoch;
  retired.master_psk_b64 = *record->master_psk_b64;
  retired.retired_at = now_ms;
  record->retired_psks.push_back(std::move(retired));
  PskBundleCodec::CapRetiredTail(record->retired_psks, detail->new_epoch);
  record->master_psk_b64 = Base64Encode(*new_psk);
  record->session_epoch = detail->new_epoch;
  record->last_psk_rotate_at = now_ms;
  record->psk_rotate_msg_count = 0;
  record->last_rotation_id = detail->rotation_id;
  if (record->key_scope == PublicKeyScope::DeviceSelf) {
    record->key_scope = PublicKeyScope::DevicePair;
  }
  if (auto saved = psk_store_.Save(*record); !saved) {
    return saved.error();
  }
  if (auto adopted = store_.AdoptChatTargetEpoch(thread_id, detail->new_epoch); !adopted) {
    return adopted.error();
  }
  return record->key_scope;
}

} // namespace pbr
