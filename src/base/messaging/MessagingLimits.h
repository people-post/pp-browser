#pragma once

#include <cstddef>

namespace pbr {

inline constexpr size_t kMaxComposeTextBytes = 64 * 1024;
inline constexpr size_t kMaxE2ePlaintextBytes = 128 * 1024;
inline constexpr size_t kMaxRelayEnvelopeBytes = 256 * 1024;
inline constexpr size_t kMaxRelayEnvelopeJsonBytes = 256 * 1024;
inline constexpr size_t kMaxPollBatchMessages = 100;
inline constexpr uint64_t kForegroundRelayPollIntervalMs = 2000;
inline constexpr int kMaxOutboxRetryAttempts = 5;
inline constexpr int kMaxGapRepairRounds = 5;
inline constexpr uint64_t kMaxGapRepairSeqSpan = 500;
inline constexpr size_t kDefaultTailSyncLimit = 50;
/** D059 — one older-history page per user-initiated sync. */
inline constexpr size_t kUserSyncOlderHistoryLimit = 25;
inline constexpr size_t kDefaultMessagesPageSize = 100;
inline constexpr size_t kMaxOpenThreadDbs = 16;
/** D042 — cap merged annotations per target message. */
inline constexpr size_t kMaxAnnotationsPerTarget = 32;

/** D040 — AI compaction (v3). */
inline constexpr int kCompactionTurnThreshold = 20;
inline constexpr size_t kMaxSummaryBytes = 8 * 1024;
inline constexpr int kCompactionMinTurnsKept = 6;

} // namespace pbr
