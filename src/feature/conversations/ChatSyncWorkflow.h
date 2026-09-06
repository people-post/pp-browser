#pragma once

#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/net/OrgBackendClients.h"
#include "domain/people/IdentityStore.h"
#include "feature/conversations/RelayReceivePipeline.h"

#include <cstdint>
#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class ContactsStore;
class InboxController;
class AttachmentFetchWorkflow;

struct ChatSyncResult {
  size_t ingested = 0;
  bool empty_gap_closed = false;
};

/** D058 unified E2E backfill — peer-direct (D060) then relay (D027). */
class ChatSyncWorkflow {
public:
  ChatSyncWorkflow(IThreadStore& store, IdentityStore& identity, ContactsStore& contacts, IRelayClient* relay,
                  RelayReceivePipeline& receive_pipeline, InboxController& inbox,
                  IChatHistoryPeerClient* peer_client = nullptr);

  Roe<ChatSyncResult> FetchChatTargetMessages(const std::string& thread_id, ChatHistoryRequest request);
  Roe<ChatSyncResult> TailSync(const std::string& thread_id);
  Roe<ChatSyncResult> RepairGap(const std::string& thread_id, uint64_t gap_min, uint64_t gap_max);
  /** D059 — tail + known gap repair + one older-history page when applicable. */
  Roe<ChatSyncResult> UserInitiatedSync(const std::string& thread_id);
  /** D059 — gap banner: repair known gap range only (not unsent outbox). */
  Roe<ChatSyncResult> RetryGapSync(const std::string& thread_id);
  /** D052 — scroll-triggered older history page. */
  Roe<ChatSyncResult> ScrollBackfill(const std::string& thread_id);

  void SetOnMessagesChanged(std::function<void()> callback);
  void SetAttachmentDownloads(AttachmentFetchWorkflow* downloads);

private:
  Roe<ChatSyncResult> RepairKnownGap(const std::string& thread_id);
  void MergeSyncResult(ChatSyncResult& aggregate, const ChatSyncResult& partial) const;
  void AdvanceContiguousThroughStoredSeqs(const std::string& thread_id, uint32_t session_epoch);
  Roe<ChatHistoryRequest> BuildRequest(const Thread& thread, uint32_t session_epoch, uint64_t history_floor_seq,
                                     std::optional<uint64_t> min_sender_seq, std::optional<uint64_t> max_sender_seq,
                                     size_t limit, const std::string& order) const;
  Roe<ChatSyncResult> IngestHistoryResponse(const std::string& thread_id, const ChatHistoryRequest& request,
                                            const ChatHistoryResponse& response,
                                            MessageTransport transport = MessageTransport::Relay);
  bool PassesEmptyGapGuard(const std::string& thread_id, uint32_t session_epoch, const std::string& seq_owner,
                           uint64_t gap_max) const;
  void CloseEmptyGap(PeerSyncState& state, uint64_t min_seq, uint64_t max_seq) const;
  std::optional<std::pair<uint64_t, uint64_t>> ClampGapRange(uint64_t gap_min, uint64_t gap_max,
                                                             uint64_t history_floor_seq) const;

  IThreadStore& store_;
  IdentityStore& identity_;
  ContactsStore& contacts_;
  IRelayClient* relay_ = nullptr;
  IChatHistoryPeerClient* peer_client_ = nullptr;
  RelayReceivePipeline& receive_pipeline_;
  InboxController& inbox_;
  AttachmentFetchWorkflow* attachment_downloads_ = nullptr;
  std::function<void()> on_messages_changed_;
};

} // namespace pbr
