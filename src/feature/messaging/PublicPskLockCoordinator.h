#pragma once

#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/PskRotateCodec.h"
#include "base/messaging/RelayEnvelope.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

enum class PublicPskRotateKind { Lock, Auto };

struct PublicPskRotatePlan {
  ChatTargetKey key;
  std::string thread_id;
  uint32_t old_epoch = 1;
  ByteVector old_master_psk;
  ByteVector new_master_psk;
  std::string key_init_b64;
  PskRotateDetail detail;
  PublicKeyScope next_scope = PublicKeyScope::DeviceSelf;
  PublicPskRotateKind kind = PublicPskRotateKind::Lock;
};

/** E027 — in-band public 1:1 device-lock / D2D auto-rekey. Not private OOB rotate. */
class PublicPskLockCoordinator {
public:
  PublicPskLockCoordinator(IThreadStore& store, IPskSessionStore& psk_store);

  Roe<PublicKeyScope> GetKeyScope(const std::string& thread_id) const;
  Roe<bool> CanLockToThisDevice(const std::string& thread_id) const;
  Roe<bool> ShouldAutoRekey(const std::string& thread_id, int64_t now_ms) const;
  Roe<void> NoteTraffic(const std::string& thread_id);

  Roe<PublicPskRotatePlan> PrepareLock(const std::string& thread_id, const ByteVector& peer_account_kem_pk,
                                       int64_t now_ms);
  Roe<PublicPskRotatePlan> PrepareAutoRekey(const std::string& thread_id, int64_t now_ms);
  Roe<void> Commit(const PublicPskRotatePlan& plan, int64_t now_ms);
  Roe<void> AbortPrepare(const PublicPskRotatePlan& plan);

  Roe<PublicKeyScope> ApplyInbound(const std::string& thread_id, const RelayEnvelope& envelope,
                                   const ThreadMessage& message, const ByteVector& local_account_kem_sk,
                                   const std::string& local_account_id, int64_t now_ms);

private:
  Roe<ChatTargetKey> TargetKeyForPublicThread(const std::string& thread_id) const;
  Roe<PskSessionRecord> LoadRequired(const ChatTargetKey& key) const;
  Roe<void> EnsureConversationKem(PskSessionRecord& record);
  Roe<PublicPskRotatePlan> PrepareRotate(const std::string& thread_id, PublicPskRotateKind kind,
                                         const ByteVector& peer_account_kem_pk, int64_t now_ms);

  IThreadStore& store_;
  IPskSessionStore& psk_store_;
};

} // namespace pbr
