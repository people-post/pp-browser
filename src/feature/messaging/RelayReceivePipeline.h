#pragma once

#include "base/crypto/ReplayWindow.h"
#include "base/crypto/IPskSessionStore.h"
#include "base/messaging/E2eIngestClassifier.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/people/IdentityStore.h"

#include <string>
#include <unordered_map>

namespace pbr {

struct RelayReceiveOutcome {
  bool persisted = false;
  bool thread_changed = false;
  IngestDecision decision = IngestDecision::HardReject;
};

/** v6 receive pipeline steps 0–12 (feature layer orchestration). */
class RelayReceivePipeline {
public:
  RelayReceivePipeline(IThreadStore& store, IPeerSigningKeyResolver& signing_keys, IPskSessionStore& psk_store,
                       IdentityStore& identity);

  RelayReceiveOutcome ProcessEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                      bool authorized_older_backfill = false,
                                      MessageTransport transport = MessageTransport::Relay);

private:
  struct ReplayKey {
    std::string thread_id;
    uint32_t session_epoch = 0;

    bool operator==(const ReplayKey& other) const {
      return thread_id == other.thread_id && session_epoch == other.session_epoch;
    }
  };

  struct ReplayKeyHash {
    size_t operator()(const ReplayKey& key) const {
      return std::hash<std::string>{}(key.thread_id) ^ (static_cast<size_t>(key.session_epoch) << 1);
    }
  };

  Roe<bool> VerifySignature(const RelayEnvelope& envelope, const DirectChatTarget& target) const;
  Roe<void> PersistDerivedAutoKeyPsk(const RelayEnvelope& envelope, const ChatTargetKey& target_key,
                                     const ByteVector& master_psk) const;
  std::optional<std::string> FindMessageIdAtSeq(const std::string& thread_id, const uint32_t session_epoch,
                                                const std::string& seq_owner_contact_id,
                                                const uint64_t sender_seq) const;
  ReplayWindow& ReplayWindowFor(const std::string& thread_id, const uint32_t session_epoch);

  IThreadStore& store_;
  IPeerSigningKeyResolver& signing_keys_;
  IPskSessionStore& psk_store_;
  IdentityStore& identity_;
  std::unordered_map<ReplayKey, ReplayWindow, ReplayKeyHash> replay_windows_;
};

} // namespace pbr
