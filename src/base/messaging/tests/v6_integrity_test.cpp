#include "base/crypto/CryptoConstants.h"
#include "base/messaging/E2eIntegrityUtil.h"
#include "base/messaging/E2eIngestClassifier.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/messaging/SyncStateCodec.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

DirectChatTarget MakeTarget(const std::string& peer) {
  DirectChatTarget target;
  target.peer_identity_kind = "relay_user";
  target.peer_identity_value = peer;
  target.channel = ThreadChannel::E2e;
  return target;
}

ThreadMessage MakeOutbound(SqliteThreadStore& store, const std::string& thread_id, const std::string& id,
                           uint64_t seq, uint32_t epoch, MessageDelivery delivery) {
  ThreadMessage message;
  message.id = id;
  message.thread_id = thread_id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = "out-" + id;
  message.timestamp = static_cast<int64_t>(seq);
  message.delivery = delivery;
  message.relay_visible = true;
  message.transport = MessageTransport::Relay;
  message.sender_seq = seq;
  message.session_epoch = epoch;
  EXPECT_TRUE(static_cast<bool>(store.AppendMessage(message)));
  return message;
}

} // namespace

TEST(V6IntegrityTest, BumpLocalEpochCancelsOldPendingAndResetsSeq) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_v6_integrity_bump_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());

  auto thread = store.FindOrCreateDirectThread(MakeTarget("relay:integrity-a"), "contact-a", "A");
  ASSERT_TRUE(static_cast<bool>(thread));

  MakeOutbound(store, thread->id, "pending-old", 1, 1, MessageDelivery::Pending);
  MakeOutbound(store, thread->id, "failed-old", 2, 1, MessageDelivery::Failed);
  MakeOutbound(store, thread->id, "relayed-old", 3, 1, MessageDelivery::Relayed);

  auto new_epoch = store.BumpLocalChatTargetEpoch(thread->id);
  ASSERT_TRUE(static_cast<bool>(new_epoch));
  EXPECT_EQ(*new_epoch, 2u);
  EXPECT_EQ(*store.GetChatTargetSessionEpoch(thread->id), 2u);

  auto seq = store.AllocateSenderSeq(thread->id);
  ASSERT_TRUE(static_cast<bool>(seq));
  EXPECT_EQ(*seq, 1u);

  auto messages = store.GetMessages(thread->id);
  ASSERT_TRUE(static_cast<bool>(messages));
  ASSERT_EQ(messages->size(), 1u);
  EXPECT_EQ(messages->front().id, "relayed-old");

  auto outbox = store.ListPendingOutbox();
  ASSERT_TRUE(static_cast<bool>(outbox));
  EXPECT_TRUE(outbox->empty());
}

TEST(V6IntegrityTest, PassiveEpochAdoptUpdatesChatTargetAndSyncState) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_v6_integrity_passive_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());

  auto thread = store.FindOrCreateDirectThread(MakeTarget("relay:integrity-b"), "contact-b", "B");
  ASSERT_TRUE(static_cast<bool>(thread));

  MakeOutbound(store, thread->id, "local-pending", 1, 1, MessageDelivery::Pending);

  PeerSyncState adopted_state = DefaultPeerSyncState();
  adopted_state.contiguous_peer_seq = 1;
  adopted_state.loaded_min_seq = 1;
  adopted_state.loaded_max_seq = 1;

  ThreadMessage inbound;
  inbound.id = "peer-epoch2-1";
  inbound.thread_id = thread->id;
  inbound.sender_contact_id = "contact-b";
  inbound.text = "hello epoch 2";
  inbound.timestamp = 10;
  inbound.delivery = MessageDelivery::Relayed;
  inbound.relay_visible = true;
  inbound.transport = MessageTransport::Relay;
  inbound.sender_seq = 1;
  inbound.session_epoch = 2;

  auto appended = store.AppendMessageWithPassiveEpochAdopt(inbound, 1, 2, adopted_state);
  ASSERT_TRUE(static_cast<bool>(appended));
  EXPECT_EQ(*store.GetChatTargetSessionEpoch(thread->id), 2u);

  auto messages = store.GetMessages(thread->id);
  ASSERT_TRUE(static_cast<bool>(messages));
  ASSERT_EQ(messages->size(), 1u);
  EXPECT_EQ(messages->front().id, "peer-epoch2-1");

  auto sync_state = store.GetPeerSyncState(thread->id, 2);
  ASSERT_TRUE(static_cast<bool>(sync_state));
  EXPECT_EQ(sync_state->contiguous_peer_seq, 1u);
  EXPECT_EQ(sync_state->phase, PeerSyncPhase::Ok);
}

TEST(V6IntegrityTest, CompromisedHelperReflectsSyncState) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_v6_integrity_compromised_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());

  auto thread = store.FindOrCreateDirectThread(MakeTarget("relay:integrity-c"), "contact-c", "C");
  ASSERT_TRUE(static_cast<bool>(thread));
  EXPECT_FALSE(IsE2eThreadCompromised(store, thread->id));

  PeerSyncState compromised = DefaultPeerSyncState();
  compromised.phase = PeerSyncPhase::Compromised;
  compromised.user_resolution = "pause_only";
  ASSERT_TRUE(static_cast<bool>(store.SetPeerSyncState(thread->id, 1, compromised)));
  EXPECT_TRUE(IsE2eThreadCompromised(store, thread->id));
}

TEST(V6IntegrityTest, ClassifierEpochAdvanceSetsPassiveAdoptDecision) {
  ReplayWindow replay_window(kReplayWindowSize);
  IngestClassifierInput input;
  input.sender_seq = 1;
  input.session_epoch = 2;
  input.message_id = "peer-msg";
  input.sync_state = DefaultPeerSyncState();
  input.chat_target_epoch = 1;
  input.strict_mode = true;

  const IngestClassifierResult result = E2eIngestClassifier::Classify(input, replay_window);
  EXPECT_EQ(result.decision, IngestDecision::AcceptEpochAdvance);
  EXPECT_TRUE(result.persist_message);
  EXPECT_EQ(result.sync_state.contiguous_peer_seq, 1u);
}
