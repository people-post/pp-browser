#include "base/messaging/E2eIngestClassifier.h"

#include "base/crypto/CryptoConstants.h"

#include <algorithm>

namespace pbr {

namespace {

bool IsLateFill(const PeerSyncState& state, const uint64_t sender_seq) {
  return std::find(state.empty_closed_seqs.begin(), state.empty_closed_seqs.end(), sender_seq) !=
         state.empty_closed_seqs.end();
}

void RemoveLateFillSeq(PeerSyncState& state, const uint64_t sender_seq) {
  auto& seqs = state.empty_closed_seqs;
  seqs.erase(std::remove(seqs.begin(), seqs.end(), sender_seq), seqs.end());
}

} // namespace

void E2eIngestClassifier::ApplyContiguousAdvance(PeerSyncState& state, const uint64_t sender_seq) {
  if (sender_seq > state.contiguous_peer_seq) {
    state.contiguous_peer_seq = sender_seq;
  }
  if (state.loaded_min_seq == 0 || sender_seq < state.loaded_min_seq) {
    state.loaded_min_seq = sender_seq;
  }
  if (sender_seq > state.loaded_max_seq) {
    state.loaded_max_seq = sender_seq;
  }
}

void E2eIngestClassifier::ApplyPersistedMessage(PeerSyncState& state, const uint64_t sender_seq) {
  ApplyContiguousAdvance(state, sender_seq);
  RemoveLateFillSeq(state, sender_seq);
  if (state.phase == PeerSyncPhase::Gap && sender_seq == state.contiguous_peer_seq) {
    state.phase = PeerSyncPhase::Ok;
  }
}

void E2eIngestClassifier::ApplyBackfillMessage(PeerSyncState& state, const uint64_t sender_seq) {
  if (state.loaded_min_seq == 0 || sender_seq < state.loaded_min_seq) {
    state.loaded_min_seq = sender_seq;
  }
  if (sender_seq > state.loaded_max_seq) {
    state.loaded_max_seq = sender_seq;
  }
}

IngestClassifierResult E2eIngestClassifier::Classify(const IngestClassifierInput& input,
                                                     ReplayWindow& replay_window) {
  IngestClassifierResult result;
  result.sync_state = input.sync_state;

  if (input.has_message_id) {
    result.decision = IngestDecision::BenignDuplicate;
    return result;
  }

  if (input.sender_seq == 0) {
    result.decision = IngestDecision::HardReject;
    return result;
  }

  if (input.sender_seq <= input.sync_state.history_floor_seq) {
    result.decision = IngestDecision::SilentDiscard;
    return result;
  }

  if (input.session_epoch < input.chat_target_epoch) {
    result.decision = IngestDecision::HardReject;
    return result;
  }

  if (input.sync_state.phase == PeerSyncPhase::Compromised) {
    result.decision = IngestDecision::SoftCompromised;
    return result;
  }

  if (input.existing_message_id_at_seq && *input.existing_message_id_at_seq != input.message_id) {
    result.decision = IngestDecision::SoftCompromised;
    result.sync_state.phase = PeerSyncPhase::Compromised;
    return result;
  }

  if (input.session_epoch > input.chat_target_epoch) {
    result.decision = IngestDecision::AcceptEpochAdvance;
    result.persist_message = true;
    result.sync_state = DefaultPeerSyncState();
    result.sync_state.contiguous_peer_seq = input.sender_seq;
    result.sync_state.loaded_min_seq = input.sender_seq;
    result.sync_state.loaded_max_seq = input.sender_seq;
    replay_window = ReplayWindow(kReplayWindowSize);
    replay_window.Accept(input.sender_seq);
    return result;
  }

  if (IsLateFill(input.sync_state, input.sender_seq)) {
    result.decision = IngestDecision::AcceptLateFill;
    result.persist_message = true;
    ApplyPersistedMessage(result.sync_state, input.sender_seq);
    replay_window.Accept(input.sender_seq);
    return result;
  }

  if (input.sender_seq < input.sync_state.contiguous_peer_seq) {
    if (input.authorized_older_backfill && input.sender_seq > input.sync_state.history_floor_seq &&
        (input.sync_state.loaded_min_seq == 0 || input.sender_seq < input.sync_state.loaded_min_seq) &&
        !input.existing_message_id_at_seq) {
      result.decision = IngestDecision::AcceptBackfill;
      result.persist_message = true;
      ApplyBackfillMessage(result.sync_state, input.sender_seq);
      return result;
    }
    result.decision = IngestDecision::SoftCompromised;
    result.sync_state.phase = PeerSyncPhase::Compromised;
    return result;
  }

  if (input.sync_state.contiguous_peer_seq == 0 && input.sync_state.loaded_max_seq == 0) {
    result.decision = IngestDecision::AcceptBootstrap;
    result.persist_message = true;
    ApplyPersistedMessage(result.sync_state, input.sender_seq);
    replay_window.Accept(input.sender_seq);
    return result;
  }

  if (input.sender_seq == input.sync_state.contiguous_peer_seq + 1) {
    result.decision = IngestDecision::AcceptContiguous;
    result.persist_message = true;
    ApplyPersistedMessage(result.sync_state, input.sender_seq);
    replay_window.Accept(input.sender_seq);
    return result;
  }

  if (input.sender_seq == 1 && input.sync_state.contiguous_peer_seq > 0) {
    result.decision = IngestDecision::SoftCompromised;
    result.sync_state.phase = PeerSyncPhase::Compromised;
    return result;
  }

  if (input.sender_seq > input.sync_state.contiguous_peer_seq + 1) {
    if (replay_window.Accept(input.sender_seq)) {
      result.decision = IngestDecision::AcceptGap;
      result.persist_message = true;
      result.sync_state.phase = PeerSyncPhase::Gap;
      if (input.sender_seq > result.sync_state.loaded_max_seq) {
        result.sync_state.loaded_max_seq = input.sender_seq;
      }
      if (result.sync_state.loaded_min_seq == 0 || input.sender_seq < result.sync_state.loaded_min_seq) {
        result.sync_state.loaded_min_seq = input.sender_seq;
      }
      if (replay_window.LastContiguous() > result.sync_state.contiguous_peer_seq) {
        ApplyContiguousAdvance(result.sync_state, replay_window.LastContiguous());
        if (result.sync_state.contiguous_peer_seq == replay_window.LastContiguous()) {
          result.sync_state.phase = PeerSyncPhase::Ok;
        }
      }
      return result;
    }
    result.decision = IngestDecision::SoftCompromised;
    if (input.strict_mode) {
      result.sync_state.phase = PeerSyncPhase::Compromised;
    }
    return result;
  }

  result.decision = IngestDecision::HardReject;
  return result;
}

} // namespace pbr
