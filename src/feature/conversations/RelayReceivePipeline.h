#pragma once

#include "foundation/crypto/ReplayWindow.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "domain/messaging/E2eIngestClassifier.h"
#include "domain/messaging/PublicPskLockCoordinator.h"
#include "domain/messaging/GroupE2ePayloadCodec.h"
#include "domain/messaging/GroupRosterStore.h"
#include "common/thread/IThreadStore.h"
#include "domain/messaging/PeerSigningKeyStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/people/IdentityStore.h"
#include "feature/calls/CallControlInboundPorts.h"
#include "common/Module.h"

#include <optional>
#include <string>
#include <unordered_map>
#include "common/PbrCompat.h"

namespace pbr {

class GroupInviteGate;
class InitiationBillingStore;
class PaymentPromiseStore;

struct RelayReceiveOutcome {
  bool persisted = false;
  bool thread_changed = false;
  IngestDecision decision = IngestDecision::HardReject;
  /** Set when `persisted` is true. */
  std::string thread_id;
  /**
   * When set, the local user (owner) should publish owner-signed member_joined after ingest.
   * Filled on successful group_invite_accept apply.
   */
  std::optional<std::string> publish_member_joined_group_id;
  std::optional<std::string> publish_member_joined_member_identity;
  uint64_t publish_member_joined_epoch = 0;
  /**
   * When an envelope from a known peer could not be read/applied: short UI notice + optional
   * thread to attach a local system line. `receive_failure_detail` is for logs.
   */
  std::optional<std::string> receive_failure_notice;
  std::optional<std::string> receive_failure_thread_id;
  std::optional<std::string> receive_failure_sender;
  std::string receive_failure_detail;
};

/** v6 receive pipeline steps 0–12 (feature layer orchestration). */
class RelayReceivePipeline : public Module {
public:
  RelayReceivePipeline(IThreadStore& store, IPeerSigningKeyResolver& signing_keys, IPskSessionStore& psk_store,
                       IdentityStore& identity, GroupRosterStore& group_roster,
                       GroupInviteGate* invite_gate = nullptr);

  void BindCallControlInbound(CallControlInboundPorts ports) { call_control_ = std::move(ports); }
  void SetInitiationBillingStore(InitiationBillingStore* store) { initiation_billing_ = store; }
  void SetPaymentPromiseStore(PaymentPromiseStore* store) { payment_promises_ = store; }

  RelayReceiveOutcome ProcessEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                      bool authorized_older_backfill = false,
                                      MessageTransport transport = MessageTransport::Relay);

private:
  RelayReceiveOutcome ProcessDirectEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                            bool authorized_older_backfill, MessageTransport transport);
  RelayReceiveOutcome ProcessGroupEnvelope(const RelayEnvelope& envelope, const std::string& local_relay_user_id,
                                           bool authorized_older_backfill, MessageTransport transport);
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
  Roe<void> ApplyInboundMembershipMessage(ThreadMessage& message, const std::string& actor_identity,
                                          RelayReceiveOutcome* outcome) const;
  Roe<void> ApplyInboundCallMessage(ThreadMessage& message, const std::string& actor_identity,
                                    std::optional<int64_t> relay_created_at_ms = std::nullopt,
                                    std::optional<int64_t> relay_server_time_ms = std::nullopt) const;
  Roe<void> ApplyInboundBillingMessage(ThreadMessage& message, const std::string& actor_identity) const;
  /** P003: stage remote payment-promise receipts; never auto-commit. */
  Roe<void> ApplyInboundPaymentPromiseMessage(ThreadMessage& message) const;
  Roe<void> ValidateInboundPskRotate(const RelayEnvelope& envelope, const ThreadMessage& message) const;
  Roe<void> ApplyInboundPskRotate(const std::string& thread_id, const RelayEnvelope& envelope,
                                 const ThreadMessage& message);

  IThreadStore& store_;
  IPeerSigningKeyResolver& signing_keys_;
  IPskSessionStore& psk_store_;
  IdentityStore& identity_;
  GroupRosterStore& group_roster_;
  GroupInviteGate* invite_gate_ = nullptr;
  CallControlInboundPorts call_control_;
  InitiationBillingStore* initiation_billing_ = nullptr;
  PaymentPromiseStore* payment_promises_ = nullptr;
  PublicPskLockCoordinator public_lock_;
  std::unordered_map<ReplayKey, ReplayWindow, ReplayKeyHash> replay_windows_;
};

} // namespace pbr
