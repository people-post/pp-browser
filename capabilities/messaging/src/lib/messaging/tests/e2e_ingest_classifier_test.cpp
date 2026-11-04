#include "lib/delivery/E2eIngestClassifier.h"
#include "lib/delivery/E2eIngestClassifier.h"

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
  input.strict_mode = true;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::SoftCompromised);
  EXPECT_EQ(result.sync_state.phase, PeerSyncPhase::Compromised);
}

TEST(E2eIngestClassifierTest, RelaxedSeqConflictKeepsFirstSeen) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 2;
  input.session_epoch = 1;
  input.message_id = "msg-new";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 2;
  input.existing_message_id_at_seq = "msg-old";
  input.strict_mode = false;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::SilentDiscard);
  EXPECT_NE(result.sync_state.phase, PeerSyncPhase::Compromised);
}

TEST(E2eIngestClassifierTest, RelaxedClearsCompromisedLatchAndAcceptsContiguous) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 6;
  input.session_epoch = 1;
  input.message_id = "msg-6";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 5;
  input.sync_state.loaded_min_seq = 1;
  input.sync_state.loaded_max_seq = 5;
  input.sync_state.phase = PeerSyncPhase::Compromised;
  input.strict_mode = false;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::AcceptContiguous);
  EXPECT_TRUE(result.persist_message);
  EXPECT_EQ(result.sync_state.phase, PeerSyncPhase::Ok);
  EXPECT_EQ(result.sync_state.contiguous_peer_seq, 6u);
}

TEST(E2eIngestClassifierTest, StrictCompromisedLatchBlocksFurtherIngest) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 6;
  input.session_epoch = 1;
  input.message_id = "msg-6";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 5;
  input.sync_state.phase = PeerSyncPhase::Compromised;
  input.strict_mode = true;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::SoftCompromised);
  EXPECT_FALSE(result.persist_message);
}

TEST(E2eIngestClassifierTest, RelaxedRewindAcceptsWithoutLoweringContiguous) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 2;
  input.session_epoch = 1;
  input.message_id = "msg-2";
  input.chat_target_epoch = 1;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 5;
  input.sync_state.loaded_min_seq = 3;
  input.sync_state.loaded_max_seq = 5;
  input.strict_mode = false;

  ReplayWindow window(32);
  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, window);
  EXPECT_EQ(result.decision, IngestDecision::AcceptBackfill);
  EXPECT_TRUE(result.persist_message);
  EXPECT_EQ(result.sync_state.contiguous_peer_seq, 5u);
  EXPECT_EQ(result.sync_state.loaded_min_seq, 2u);
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

// PB-064: replay windows live in memory; after a restart the window starts at 0 while the persisted
// state says 50 messages came in order. A strict private chat's message 52 arriving before 51 is a gap
// to fill, not a sign the chat was tampered with.
TEST(E2eIngestClassifierTest, AGapAfterARestartIsAGapNotACompromise) {
  using namespace pbr;

  IngestClassifierInput input;
  input.sender_seq = 52;
  input.session_epoch = 1;
  input.message_id = "msg-52";
  input.chat_target_epoch = 1;
  input.strict_mode = true;
  input.sync_state = DefaultPeerSyncState();
  input.sync_state.contiguous_peer_seq = 50;
  input.sync_state.loaded_max_seq = 50;

  ReplayWindow fresh(32);  // what a restarted app holds
  const IngestClassifierResult gap = E2eIngestClassifier::Classify(input, fresh);
  EXPECT_EQ(gap.decision, IngestDecision::AcceptGap);
  EXPECT_NE(gap.sync_state.phase, PeerSyncPhase::Compromised);

  input.sender_seq = 51;
  input.message_id = "msg-51";
  input.sync_state = gap.sync_state;
  const IngestClassifierResult filled = E2eIngestClassifier::Classify(input, fresh);
  EXPECT_TRUE(filled.persist_message);
  EXPECT_EQ(filled.sync_state.contiguous_peer_seq, 52u) << "51 closes the gap up to 52";
}
