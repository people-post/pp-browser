#pragma once

#include "base/crypto/ReplayWindow.h"
#include "base/messaging/SyncStateTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

enum class IngestDecision {
  BenignDuplicate,
  SilentDiscard,
  AcceptContiguous,
  AcceptBootstrap,
  AcceptLateFill,
  AcceptBackfill,
  AcceptEpochAdvance,
  AcceptGap,
  SoftCompromised,
  HardReject,
};

struct IngestClassifierInput {
  uint64_t sender_seq = 0;
  uint32_t session_epoch = 1;
  std::string message_id;
  PeerSyncState sync_state;
  uint32_t chat_target_epoch = 1;
  bool strict_mode = true;
  std::optional<std::string> existing_message_id_at_seq;
  bool has_message_id = false;
  /** D059/D058 — authorized older-history fetch below contiguous tail. */
  bool authorized_older_backfill = false;
};

struct IngestClassifierResult {
  IngestDecision decision = IngestDecision::HardReject;
  PeerSyncState sync_state;
  bool persist_message = false;
};

/** D013 ingest classifier — authoritative over ReplayWindow (D020). */
class E2eIngestClassifier {
public:
  static IngestClassifierResult Classify(const IngestClassifierInput& input, ReplayWindow& replay_window);

  static void ApplyContiguousAdvance(PeerSyncState& state, const uint64_t sender_seq);
  static void ApplyPersistedMessage(PeerSyncState& state, const uint64_t sender_seq);
  static void ApplyBackfillMessage(PeerSyncState& state, const uint64_t sender_seq);
};

} // namespace pbr
