// The chat-history sync rules over a thread's peer watermarks (ChatSyncWorkflow runs the fetches).
#include "domain/messaging/ChatSyncLogic.h"

#include "common/chat/MessagingLimits.h"

#include <gtest/gtest.h>

#include <set>

namespace pbr {
namespace {

using chat_sync::SeqRange;

TEST(ChatSyncLogicTest, WhatEachSyncAsksFor) {
  PeerSyncState state;
  EXPECT_EQ(chat_sync::TailMinSeq(state), std::nullopt) << "nothing loaded: everything";
  state.loaded_min_seq = 40;
  state.loaded_max_seq = 50;
  state.contiguous_peer_seq = 44;
  EXPECT_EQ(chat_sync::TailMinSeq(state), std::optional<uint64_t>(51));
  EXPECT_EQ(chat_sync::KnownGap(state), std::nullopt) << "not in Gap";
  state.phase = PeerSyncPhase::Gap;
  EXPECT_EQ(chat_sync::KnownGap(state), (SeqRange{45, 49}));
  state.contiguous_peer_seq = 49;
  EXPECT_EQ(chat_sync::KnownGap(state), std::nullopt) << "only the newest is past the run";

  EXPECT_EQ(chat_sync::OlderBackfillMaxSeq(state), std::optional<uint64_t>(39));
  state.history_floor_seq = 39;
  EXPECT_EQ(chat_sync::OlderBackfillMaxSeq(state), std::nullopt) << "at the floor";
}

TEST(ChatSyncLogicTest, AGapRepairStaysAboveTheFloorAndBounded) {
  EXPECT_EQ(chat_sync::ClampGapRange(5, 4, 0), std::nullopt);
  EXPECT_EQ(chat_sync::ClampGapRange(1, 10, 0), (SeqRange{1, 10}));
  EXPECT_EQ(chat_sync::ClampGapRange(1, 10, 6), (SeqRange{7, 10}));
  EXPECT_EQ(chat_sync::ClampGapRange(1, 10, 10), std::nullopt) << "all below the floor";
  EXPECT_EQ(chat_sync::ClampGapRange(1, 10000, 0), (SeqRange{1, kMaxGapRepairSeqSpan})) << "the rest next time";
}

TEST(ChatSyncLogicTest, RequestBoundsAndAnswerCap) {
  ChatHistoryRequest request;
  chat_sync::BoundHistoryRequest(request, 10, std::optional<uint64_t>(3), std::optional<uint64_t>(20), 1000);
  EXPECT_EQ(request.min_sender_seq, std::optional<uint64_t>(11)) << "never at or below the floor";
  EXPECT_EQ(request.max_sender_seq, std::optional<uint64_t>(20));
  EXPECT_EQ(request.limit, kMaxPollBatchMessages);
  chat_sync::BoundHistoryRequest(request, 0, std::nullopt, std::nullopt, 7);
  EXPECT_EQ(request.min_sender_seq, std::nullopt);
  EXPECT_EQ(chat_sync::HistoryResponseCap(request), 7u);
  request.limit = 0;
  EXPECT_EQ(chat_sync::HistoryResponseCap(request), kMaxPollBatchMessages);

  PeerSyncState state;
  state.loaded_min_seq = 40;
  request.max_sender_seq = 39;
  EXPECT_TRUE(chat_sync::AuthorizedOlderBackfill(request, state));
  request.max_sender_seq = 40;
  EXPECT_FALSE(chat_sync::AuthorizedOlderBackfill(request, state));
}

TEST(ChatSyncLogicTest, WatermarksMove) {
  PeerSyncState state;
  state.phase = PeerSyncPhase::Gap;
  state.contiguous_peer_seq = 3;
  state.loaded_max_seq = 9;
  chat_sync::CloseEmptyGap(state, 4, 5);
  EXPECT_EQ(state.contiguous_peer_seq, 5u);
  EXPECT_EQ(state.empty_closed_seqs, (std::vector<uint64_t>{4, 5}));
  EXPECT_EQ(state.phase, PeerSyncPhase::Ok);

  state.phase = PeerSyncPhase::Gap;
  const std::set<uint64_t> stored{6, 7, 9};
  EXPECT_TRUE(chat_sync::AdvanceContiguous(state, [&](uint64_t seq) { return stored.count(seq) > 0; }));
  EXPECT_EQ(state.contiguous_peer_seq, 7u) << "stops at the first missing";
  EXPECT_EQ(state.phase, PeerSyncPhase::Gap);
  EXPECT_FALSE(chat_sync::AdvanceContiguous(state, [&](uint64_t seq) { return stored.count(seq) > 0; }));
  EXPECT_TRUE(chat_sync::AdvanceContiguous(state, [](uint64_t) { return true; }));
  EXPECT_EQ(state.contiguous_peer_seq, 9u);
  EXPECT_EQ(state.phase, PeerSyncPhase::Ok);

  Thread thread;
  thread.peer_identity_value = "account:p";
  EXPECT_EQ(chat_sync::SeqOwner(thread), "account:p");
  thread.participant_contact_ids = {"contact-1"};
  EXPECT_EQ(chat_sync::SeqOwner(thread), "contact-1");
}

} // namespace
} // namespace pbr
