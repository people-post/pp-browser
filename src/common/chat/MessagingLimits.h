#pragma once

#include <cstddef>
#include <cstdint>

namespace pbr {

inline constexpr size_t kMaxComposeTextBytes = 64 * 1024;
inline constexpr size_t kMaxE2ePlaintextBytes = 128 * 1024;
inline constexpr size_t kMaxRelayEnvelopeBytes = 256 * 1024;
inline constexpr size_t kMaxRelayEnvelopeJsonBytes = 256 * 1024;
inline constexpr size_t kMaxPollBatchMessages = 100;
inline constexpr uint64_t kForegroundRelayPollIntervalMs = 2000;
/** D032 / P006 — in-process background poll while app is alive. */
inline constexpr uint64_t kBackgroundRelayPollIntervalMs = 45000;
/** WorkManager backup when alerts on (milliseconds). */
inline constexpr uint64_t kWorkManagerAlertsOnBackupIntervalMs = 3ULL * 60ULL * 60ULL * 1000ULL;
/** WorkManager when alerts off (~15 min Android minimum). */
inline constexpr uint64_t kWorkManagerAlertsOffIntervalMs = 15ULL * 60ULL * 1000ULL;
inline constexpr int kMaxOutboxRetryAttempts = 5;
inline constexpr int kMaxGapRepairRounds = 5;
inline constexpr uint64_t kMaxGapRepairSeqSpan = 500;
inline constexpr size_t kDefaultTailSyncLimit = 50;
/** D059 — one older-history page per user-initiated sync. */
inline constexpr size_t kUserSyncOlderHistoryLimit = 25;
inline constexpr size_t kDefaultMessagesPageSize = 100;
/** Cap on chat transcript rows kept in the Rml `messages` data-for window. */
inline constexpr size_t kMaxMessagesDomWindow = 200;
inline constexpr size_t kMaxOpenThreadDbs = 16;
/** D042 — cap merged annotations per target message. */
inline constexpr size_t kMaxAnnotationsPerTarget = 32;

/** R008 / R021 — Soft max plaintext attachment size for Smart auto-download. */
inline constexpr uint64_t kMaxChatAttachmentPlaintextBytes = 4ULL * 1024ULL * 1024ULL;

/** D040 — AI compaction (v3). */
inline constexpr int kCompactionTurnThreshold = 20;
inline constexpr size_t kMaxSummaryBytes = 8 * 1024;
inline constexpr int kCompactionMinTurnsKept = 6;

} // namespace pbr
