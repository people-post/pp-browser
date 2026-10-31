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
/**
 * In the background a phone is reached by push only (power): its work is paused (AppLifecycle) and
 * the relay poll with it. A push / WorkManager wake resumes the work for this long — enough to fetch
 * the inbox and ring — then it pauses again.
 */
inline constexpr uint64_t kBackgroundWakeWindowMs = 20000;
/** P011 — staying connected in the background (opt-in, Android): the relay poll's pace there. */
inline constexpr uint64_t kStayConnectedRelayPollIntervalMs = 30000;
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
/**
 * Rows dropped at once when a pinned transcript goes over the cap. The list is bound by index, so every
 * drop makes the UI re-parse all rows; dropping one row per new message meant that for every message.
 */
inline constexpr size_t kMessagesDomWindowTrimBatch = 50;
inline constexpr size_t kMaxOpenThreadDbs = 16;
/** D042 — cap merged annotations per target message. */
inline constexpr size_t kMaxAnnotationsPerTarget = 32;

/**
 * Largest plaintext attachment sent or answered peer-to-peer (`/pp/blob/1.0.0`, 15 s timeout), whatever the
 * account's tier: a bigger one goes through the server only. The send, receive and auto-download limits live
 * in `AttachmentLimits` (lib/messaging).
 */
inline constexpr uint64_t kMaxDirectAttachmentBytes = 4ULL * 1024ULL * 1024ULL;

/** D040 — AI compaction (v3). */
inline constexpr int kCompactionTurnThreshold = 20;
inline constexpr size_t kMaxSummaryBytes = 8 * 1024;
inline constexpr int kCompactionMinTurnsKept = 6;

} // namespace pbr
