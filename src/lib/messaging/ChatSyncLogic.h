#pragma once

#include "common/thread/ChatHistoryTypes.h"
#include "common/thread/SyncStateTypes.h"
#include "common/thread/ThreadTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pbr {

/**
 * Pure chat-history sync rules over a thread's peer watermarks (D037 / D071): which sender
 * sequences a tail sync, a gap repair or an older backfill asks for, and how an answer moves the
 * watermarks. ChatSyncWorkflow runs the fetches and ingests through the receive pipeline.
 */
namespace chat_sync {

/** An inclusive range of the peer's sender sequences. */
struct SeqRange {
  uint64_t min = 0;
  uint64_t max = 0;
  bool operator==(const SeqRange&) const = default;
};

/** Whose sender sequences the thread counts: its first participant, else the peer. */
std::string SeqOwner(const Thread& thread);

/** A tail sync asks for what follows the newest loaded message (all, when none is loaded). */
std::optional<uint64_t> TailMinSeq(const PeerSyncState& state);

/** The known gap: between the contiguous run and the newest loaded message, while in Gap. */
std::optional<SeqRange> KnownGap(const PeerSyncState& state);

/**
 * A gap repair's range: above the history floor and at most kMaxGapRepairSeqSpan long (the rest
 * comes on the next repair); none when nothing above the floor is missing.
 */
std::optional<SeqRange> ClampGapRange(uint64_t gap_min, uint64_t gap_max, uint64_t history_floor_seq);

/** An older backfill asks below the oldest loaded message, down to the floor; none at the floor. */
std::optional<uint64_t> OlderBackfillMaxSeq(const PeerSyncState& state);

/** A request's bounds: never at or below the history floor; the limit at most a poll batch. */
void BoundHistoryRequest(ChatHistoryRequest& request, uint64_t history_floor_seq, std::optional<uint64_t> min_seq,
                         std::optional<uint64_t> max_seq, size_t limit);

/** No more of an answer is taken than was asked for, and never past a poll batch. */
size_t HistoryResponseCap(const ChatHistoryRequest& request);

/** The request reaches below the oldest loaded message: older rows it brings are wanted. */
bool AuthorizedOlderBackfill(const ChatHistoryRequest& request, const PeerSyncState& state);

/** The peer has nothing in [min, max] (it answered empty and has nothing after): close it. */
void CloseEmptyGap(PeerSyncState& state, uint64_t min_seq, uint64_t max_seq);

/**
 * Extend the contiguous run through sequences stored since (`stored(seq)`), and leave Gap once it
 * reaches the newest loaded message. True when the state changed.
 */
bool AdvanceContiguous(PeerSyncState& state, const std::function<bool(uint64_t)>& stored);

} // namespace chat_sync
} // namespace pbr
