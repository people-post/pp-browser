#include "base/crypto/CryptoUtil.h"
#include "base/messaging/E2eIngestClassifier.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/messaging/SyncStateCodec.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/people/Ed25519Signer.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/ChatSyncService.h"
#include "feature/messaging/RelayReceivePipeline.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>

namespace {

using namespace pbr;

class SyncTestHarness {
public:
  explicit SyncTestHarness(const std::string& suffix)
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_chat_sync_" + suffix)),
        store(data_dir.string()),
        identity(data_dir.string()),
        key_resolver(key_store),
        receive_pipeline(store, key_resolver),
        sync(store, identity, &relay, receive_pipeline, &peer_history) {
    std::filesystem::remove_all(data_dir);

    auto generated = Ed25519Signer::GenerateKeyPair();
    if (!generated) {
      throw std::runtime_error("Failed to generate peer keys");
    }
    peer_keys = *generated;
    peer_private_key = peer_keys.private_key;

    PeerSigningKeyRecord record;
    record.signing_public_key_b64 = Ed25519Signer::ToBase64(peer_keys.public_key);
    record.source = "test";
    key_store.Put("relay_user", "relay:peer", record);

    DirectChatTarget target;
    target.peer_identity_kind = "relay_user";
    target.peer_identity_value = "relay:peer";
    target.channel = ThreadChannel::E2e;

    auto created = store.FindOrCreateDirectThread(target, "contact-peer", "Peer");
    if (!created) {
      throw std::runtime_error("Failed to create thread");
    }
    thread = *created;

    if (!identity.LoadOrCreate()) {
      throw std::runtime_error("Failed to load identity");
    }
  }

  RelayEnvelope MakePeerEnvelope(uint64_t seq, const std::string& text) const {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = "peer-msg-" + std::to_string(seq);
    envelope.sender_relay_id = "relay:peer";
    envelope.sender_contact_id = "relay:peer";
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2e;
    auto payload = RelayWirePayload::EncodePlaintextText(text);
    if (!payload) {
      throw std::runtime_error("Failed to encode payload");
    }
    envelope.body.e2e.payload_b64 = *payload;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = static_cast<int64_t>(seq);
    auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
    if (!sign_bytes) {
      throw std::runtime_error("Failed to build sign bytes");
    }
    auto signature =
        Ed25519Signer::Sign(std::string(sign_bytes->begin(), sign_bytes->end()), peer_private_key);
    if (!signature) {
      throw std::runtime_error("Failed to sign envelope");
    }
    envelope.signature = *signature;
    return envelope;
  }

  void SeedPeerSeq(uint64_t seq, const std::string& text) {
    ThreadMessage message;
    message.id = "local-seed-" + std::to_string(seq);
    message.thread_id = thread.id;
    message.sender_contact_id = "contact-peer";
    message.text = text;
    message.timestamp = static_cast<int64_t>(seq);
    message.delivery = MessageDelivery::Relayed;
    message.relay_visible = true;
    message.transport = MessageTransport::Relay;
    message.sender_seq = seq;
    message.session_epoch = 1;
    if (!store.AppendMessage(message)) {
      throw std::runtime_error("Failed to append seed message");
    }

    PeerSyncState state = DefaultPeerSyncState();
    state.contiguous_peer_seq = seq;
    state.loaded_min_seq = seq;
    state.loaded_max_seq = seq;
    if (!store.SetPeerSyncState(thread.id, 1, state)) {
      throw std::runtime_error("Failed to set sync state");
    }
  }

  std::filesystem::path data_dir;
  SqliteThreadStore store;
  IdentityStore identity;
  MockRelayClient relay;
  MockChatHistoryPeerClient peer_history;
  PeerSigningKeyStore key_store;
  PeerSigningKeyResolver key_resolver;
  RelayReceivePipeline receive_pipeline;
  ChatSyncService sync;
  Thread thread;
  Ed25519KeyPair peer_keys;
  std::vector<uint8_t> peer_private_key;
};

} // namespace

TEST(ChatSyncTest, MockFetchReturnsInjectedEnvelope) {
  SyncTestHarness harness("mock_fetch");
  harness.relay.AddDeliveredEnvelope(harness.MakePeerEnvelope(2, "two"));

  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = "relay:local";
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = "relay:peer";
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.min_sender_seq = 2;
  request.max_sender_seq = 2;
  request.limit = 10;
  request.order = "asc";

  auto response = harness.relay.FetchChatHistory(request);
  ASSERT_TRUE(static_cast<bool>(response));
  ASSERT_EQ(response->messages.size(), 1u);
}

TEST(ChatSyncTest, ReceivePipelineIngestsGapFill) {
  SyncTestHarness harness("pipeline");
  harness.SeedPeerSeq(1, "one");

  const RelayReceiveOutcome outcome = harness.receive_pipeline.ProcessEnvelope(harness.MakePeerEnvelope(2, "two"));
  EXPECT_EQ(outcome.decision, IngestDecision::AcceptContiguous);
  EXPECT_TRUE(outcome.persisted);
}

TEST(ChatSyncTest, RepairGapIngestsMissingPeerMessage) {
  SyncTestHarness harness("gap_repair");
  harness.SeedPeerSeq(1, "one");

  harness.relay.AddDeliveredEnvelope(harness.MakePeerEnvelope(2, "two"));

  auto result = harness.sync.RepairGap(harness.thread.id, 2, 2);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result->ingested, 1u);
  EXPECT_FALSE(result->empty_gap_closed);

  auto messages = harness.store.GetMessages(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(messages));
  ASSERT_EQ(messages->size(), 2u);
  EXPECT_EQ(messages->back().text, "two");
  EXPECT_EQ(*messages->back().sender_seq, 2u);

  auto sync_state = harness.store.GetPeerSyncState(harness.thread.id, 1);
  ASSERT_TRUE(static_cast<bool>(sync_state));
  EXPECT_EQ(sync_state->contiguous_peer_seq, 2u);
}

TEST(ChatSyncTest, EmptyGapCloseWhenAuthoritativeGuardPasses) {
  SyncTestHarness harness("empty_gap");
  harness.SeedPeerSeq(1, "one");

  auto result = harness.sync.RepairGap(harness.thread.id, 2, 2);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result->ingested, 0u);
  EXPECT_TRUE(result->empty_gap_closed);

  auto sync_state = harness.store.GetPeerSyncState(harness.thread.id, 1);
  ASSERT_TRUE(static_cast<bool>(sync_state));
  EXPECT_EQ(sync_state->contiguous_peer_seq, 2u);
  ASSERT_EQ(sync_state->empty_closed_seqs.size(), 1u);
  EXPECT_EQ(sync_state->empty_closed_seqs.front(), 2u);
}

TEST(ChatSyncTest, EmptyGapCloseBlockedWhenHigherSeqHeld) {
  SyncTestHarness harness("empty_gap_guard_fail");
  harness.SeedPeerSeq(1, "one");

  ThreadMessage higher;
  higher.id = "local-three";
  higher.thread_id = harness.thread.id;
  higher.sender_contact_id = "contact-peer";
  higher.text = "three";
  higher.timestamp = 3;
  higher.delivery = MessageDelivery::Relayed;
  higher.relay_visible = true;
  higher.transport = MessageTransport::Relay;
  higher.sender_seq = 3;
  higher.session_epoch = 1;
  ASSERT_TRUE(static_cast<bool>(harness.store.AppendMessage(higher)));

  auto result = harness.sync.RepairGap(harness.thread.id, 2, 2);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result->ingested, 0u);
  EXPECT_FALSE(result->empty_gap_closed);

  auto sync_state = harness.store.GetPeerSyncState(harness.thread.id, 1);
  ASSERT_TRUE(static_cast<bool>(sync_state));
  EXPECT_EQ(sync_state->contiguous_peer_seq, 1u);
  EXPECT_TRUE(sync_state->empty_closed_seqs.empty());
}

TEST(ChatSyncTest, LateFillAcceptsAfterAuthoritativeEmptyClose) {
  SyncTestHarness harness("late_fill");
  harness.SeedPeerSeq(1, "one");

  auto empty_close = harness.sync.RepairGap(harness.thread.id, 2, 2);
  ASSERT_TRUE(static_cast<bool>(empty_close));
  EXPECT_TRUE(empty_close->empty_gap_closed);

  harness.relay.AddDeliveredEnvelope(harness.MakePeerEnvelope(2, "two"));
  const RelayReceiveOutcome outcome = harness.receive_pipeline.ProcessEnvelope(harness.MakePeerEnvelope(2, "two"));
  EXPECT_EQ(outcome.decision, IngestDecision::AcceptLateFill);
  EXPECT_TRUE(outcome.persisted);

  auto sync_state = harness.store.GetPeerSyncState(harness.thread.id, 1);
  ASSERT_TRUE(static_cast<bool>(sync_state));
  EXPECT_EQ(sync_state->contiguous_peer_seq, 2u);
  EXPECT_TRUE(sync_state->empty_closed_seqs.empty());
}

TEST(ChatSyncTest, UserInitiatedSyncFetchesOlderHistory) {
  SyncTestHarness harness("user_older");
  harness.SeedPeerSeq(5, "five");

  harness.relay.AddDeliveredEnvelope(harness.MakePeerEnvelope(3, "three"));

  auto result = harness.sync.UserInitiatedSync(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_GE(result->ingested, 1u);

  auto messages = harness.store.GetMessages(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(messages));
  bool found_three = false;
  for (const ThreadMessage& message : *messages) {
    if (message.text == "three") {
      found_three = true;
      break;
    }
  }
  EXPECT_TRUE(found_three);
}

TEST(ChatSyncTest, PeerDirectPreferredOverRelay) {
  SyncTestHarness harness("peer_first");
  harness.SeedPeerSeq(1, "one");

  harness.peer_history.SetPeerReachable("relay:peer");
  harness.peer_history.AddDeliveredEnvelope(harness.MakePeerEnvelope(2, "via-direct"));

  auto result = harness.sync.RepairGap(harness.thread.id, 2, 2);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result->ingested, 1u);

  auto messages = harness.store.GetMessages(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(messages));
  ASSERT_EQ(messages->size(), 2u);
  EXPECT_EQ(messages->back().text, "via-direct");
  EXPECT_EQ(*messages->back().transport, MessageTransport::Direct);
}

TEST(ChatSyncTest, RetryGapSyncRepairsKnownGap) {
  SyncTestHarness harness("retry_gap");
  harness.SeedPeerSeq(1, "one");

  const RelayReceiveOutcome gap_outcome =
      harness.receive_pipeline.ProcessEnvelope(harness.MakePeerEnvelope(3, "three"));
  EXPECT_EQ(gap_outcome.decision, IngestDecision::AcceptGap);

  harness.relay.AddDeliveredEnvelope(harness.MakePeerEnvelope(2, "two"));

  auto result = harness.sync.RetryGapSync(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result->ingested, 1u);

  auto sync_state = harness.store.GetPeerSyncState(harness.thread.id, 1);
  ASSERT_TRUE(static_cast<bool>(sync_state));
  EXPECT_EQ(sync_state->contiguous_peer_seq, 3u);
  EXPECT_EQ(sync_state->phase, PeerSyncPhase::Ok);
}
