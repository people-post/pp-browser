#include "base/crypto/ReplayWindow.h"
#include "base/messaging/E2eIngestClassifier.h"

#include <gtest/gtest.h>

TEST(E2eIngestClassifierTest, ContiguousTailAccepted) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 1;
  input.session_epoch = 1;
  input.message_id = "msg-1";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::AcceptBootstrap);
  EXPECT_TRUE(result.persist_message);
  EXPECT_EQ(result.sync_state.contiguous_peer_seq, 1u);
}

TEST(E2eIngestClassifierTest, BelowFloorSilentlyDiscarded) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 3;
  input.session_epoch = 1;
  input.message_id = "msg-3";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.history_floor_seq = 5;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::SilentDiscard);
  EXPECT_FALSE(result.persist_message);
}

TEST(E2eIngestClassifierTest, GapWithinReplayWindowAccepted) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 3;
  input.session_epoch = 1;
  input.message_id = "msg-3";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 1;
  input.sync_state.loaded_max_seq = 1;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::AcceptGap);
  EXPECT_TRUE(result.persist_message);
  EXPECT_EQ(result.sync_state.phase, PeerSyncPhase::Gap);
}

TEST(E2eIngestClassifierTest, SeqConflictMarksCompromised) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 2;
  input.session_epoch = 1;
  input.message_id = "msg-new";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 2;
  input.existing_message_id_at_seq = "msg-old";

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::SoftCompromised);
  EXPECT_EQ(result.sync_state.phase, PeerSyncPhase::Compromised);
}

TEST(E2eIngestClassifierTest, LateFillAcceptedWhenSeqWasEmptyClosed) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 2;
  input.session_epoch = 1;
  input.message_id = "msg-2";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 1;
  input.sync_state.loaded_max_seq = 1;
  input.sync_state.empty_closed_seqs = {2};

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::AcceptLateFill);
  EXPECT_TRUE(result.persist_message);
  EXPECT_EQ(result.sync_state.contiguous_peer_seq, 2u);
  EXPECT_TRUE(result.sync_state.empty_closed_seqs.empty());
}

TEST(E2eIngestClassifierTest, AuthorizedBackfillAcceptsOlderSeqBelowContiguous) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 3;
  input.session_epoch = 1;
  input.message_id = "msg-3";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 5;
  input.sync_state.loaded_min_seq = 5;
  input.sync_state.loaded_max_seq = 5;
  input.authorized_older_backfill = true;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::AcceptBackfill);
  EXPECT_TRUE(result.persist_message);
  EXPECT_EQ(result.sync_state.contiguous_peer_seq, 5u);
  EXPECT_EQ(result.sync_state.loaded_min_seq, 3u);
}
