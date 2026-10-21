#include "domain/messaging/ChatSyncLogic.h"

#include "common/chat/MessagingLimits.h"

#include <algorithm>

namespace pbr::chat_sync {

std::string SeqOwner(const Thread& thread) {
  return thread.participant_contact_ids.empty() ? thread.peer_identity_value : thread.participant_contact_ids.front();
}

std::optional<uint64_t> TailMinSeq(const PeerSyncState& state) {
  if (state.loaded_max_seq == 0) {
    return std::nullopt;
  }
  return state.loaded_max_seq + 1;
}

std::optional<SeqRange> KnownGap(const PeerSyncState& state) {
  if (state.phase != PeerSyncPhase::Gap || state.loaded_max_seq <= state.contiguous_peer_seq + 1) {
    return std::nullopt;
  }
  return SeqRange{state.contiguous_peer_seq + 1, state.loaded_max_seq - 1};
}

std::optional<SeqRange> ClampGapRange(const uint64_t gap_min, const uint64_t gap_max, const uint64_t history_floor_seq) {
  if (gap_max < gap_min) {
    return std::nullopt;
  }
  const uint64_t clamped_min = std::max(gap_min, history_floor_seq + 1);
  if (clamped_min > gap_max) {
    return std::nullopt;
  }
  return SeqRange{clamped_min, std::min(gap_max, clamped_min + kMaxGapRepairSeqSpan - 1)};
}

std::optional<uint64_t> OlderBackfillMaxSeq(const PeerSyncState& state) {
  if (state.loaded_min_seq <= state.history_floor_seq + 1) {
    return std::nullopt;
  }
  return state.loaded_min_seq - 1;
}

void BoundHistoryRequest(ChatHistoryRequest& request, const uint64_t history_floor_seq,
                         const std::optional<uint64_t> min_seq, const std::optional<uint64_t> max_seq,
                         const size_t limit) {
  request.limit = std::min(limit, kMaxPollBatchMessages);
  if (min_seq) {
    request.min_sender_seq = std::max(*min_seq, history_floor_seq + 1);
  } else if (history_floor_seq > 0) {
    request.min_sender_seq = history_floor_seq + 1;
  } else {
    request.min_sender_seq.reset();
  }
  request.max_sender_seq = max_seq;
}

size_t HistoryResponseCap(const ChatHistoryRequest& request) {
  return std::min<size_t>(request.limit == 0 ? kMaxPollBatchMessages : request.limit, kMaxPollBatchMessages);
}

bool AuthorizedOlderBackfill(const ChatHistoryRequest& request, const PeerSyncState& state) {
  return request.max_sender_seq && state.loaded_min_seq > 0 && *request.max_sender_seq < state.loaded_min_seq;
}

void CloseEmptyGap(PeerSyncState& state, const uint64_t min_seq, const uint64_t max_seq) {
  for (uint64_t seq = min_seq; seq <= max_seq; ++seq) {
    if (std::find(state.empty_closed_seqs.begin(), state.empty_closed_seqs.end(), seq) ==
        state.empty_closed_seqs.end()) {
      state.empty_closed_seqs.push_back(seq);
    }
  }
  state.contiguous_peer_seq = std::max(state.contiguous_peer_seq, max_seq);
  if (state.phase == PeerSyncPhase::Gap) {
    state.phase = PeerSyncPhase::Ok;
  }
}

bool AdvanceContiguous(PeerSyncState& state, const std::function<bool(uint64_t)>& stored) {
  bool changed = false;
  while (state.contiguous_peer_seq < state.loaded_max_seq && stored(state.contiguous_peer_seq + 1)) {
    ++state.contiguous_peer_seq;
    changed = true;
  }
  if (state.phase == PeerSyncPhase::Gap && state.contiguous_peer_seq == state.loaded_max_seq) {
    state.phase = PeerSyncPhase::Ok;
    changed = true;
  }
  return changed;
}

} // namespace pbr::chat_sync
