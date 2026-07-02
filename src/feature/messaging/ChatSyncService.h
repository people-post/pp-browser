#pragma once

#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/RelayReceivePipeline.h"

#include <cstdint>
#include <functional>
#include <string>

namespace pbr {

struct ChatSyncResult {
  size_t ingested = 0;
  bool empty_gap_closed = false;
};

/** D058 unified E2E backfill — relay transport in v6-sync (libp2p deferred to v6-libp2p). */
class ChatSyncService {
public:
  ChatSyncService(IThreadStore& store, IdentityStore& identity, IRelayClient* relay,
                  RelayReceivePipeline& receive_pipeline);

  Roe<ChatSyncResult> FetchChatTargetMessages(const std::string& thread_id, ChatHistoryRequest request);
  Roe<ChatSyncResult> TailSync(const std::string& thread_id);
  Roe<ChatSyncResult> RepairGap(const std::string& thread_id, uint64_t gap_min, uint64_t gap_max);

  void SetOnMessagesChanged(std::function<void()> callback);

private:
  Roe<ChatHistoryRequest> BuildRequest(const Thread& thread, uint32_t session_epoch, uint64_t history_floor_seq,
                                     std::optional<uint64_t> min_sender_seq, std::optional<uint64_t> max_sender_seq,
                                     size_t limit, const std::string& order) const;
  Roe<ChatSyncResult> IngestHistoryResponse(const std::string& thread_id, const ChatHistoryRequest& request,
                                            const ChatHistoryResponse& response);
  bool PassesEmptyGapGuard(const std::string& thread_id, uint32_t session_epoch, const std::string& seq_owner,
                           uint64_t gap_max) const;
  void CloseEmptyGap(PeerSyncState& state, uint64_t min_seq, uint64_t max_seq) const;
  std::optional<std::pair<uint64_t, uint64_t>> ClampGapRange(uint64_t gap_min, uint64_t gap_max,
                                                             uint64_t history_floor_seq) const;

  IThreadStore& store_;
  IdentityStore& identity_;
  IRelayClient* relay_ = nullptr;
  RelayReceivePipeline& receive_pipeline_;
  std::function<void()> on_messages_changed_;
};

} // namespace pbr
