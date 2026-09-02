#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "common/chat/RelayStreamKey.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/messaging/SyncStateCodec.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/people/ContactsStore.h"
#include "base/crypto/MlDsa.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/ChatSyncService.h"
#include "feature/messaging/DirectoryShadowCache.h"
#include "feature/messaging/InboxController.h"
#include "feature/messaging/PeerDisplayResolver.h"
#include "base/messaging/GroupRosterStore.h"
#include "feature/messaging/RelayReceivePipeline.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>

namespace {

using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

class SyncTestHarness {
public:
  explicit SyncTestHarness(const std::string& suffix)
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_chat_sync_" + suffix)),
        store(data_dir.string()),
        identity(data_dir.string(), "test"),
        psk_store(store.ProfileDbPath(), "test"),
        key_resolver(key_store),
        roster_store(store.ProfileDbPath()),
        contacts(data_dir.string()),
        shadows(directory),
        labels(contacts, shadows),
        inbox(store, contacts, labels, &shadows),
        receive_pipeline(store, key_resolver, psk_store, identity, roster_store),
        sync(store, identity, contacts, &relay, receive_pipeline, inbox, &peer_history) {
    std::filesystem::remove_all(data_dir);
    if (!identity.SetDek(TestDek()) || !psk_store.SetDek(TestDek()) || !store.SetDek(TestDek())) {
      throw std::runtime_error("Failed to set test DEK");
    }

    auto generated = MlDsa::GenerateKeyPair();
    if (!generated) {
      throw std::runtime_error("Failed to generate peer keys");
    }
    peer_keys = *generated;
    peer_private_key = peer_keys.secret_key;

    PeerSigningKeyRecord record;
    record.signing_public_key_b64 = Base64Encode(peer_keys.public_key);
    record.source = "test";
    key_store.Put("account", "account:peer", record);

    Contact peer_contact;
    peer_contact.id = "contact-peer";
    peer_contact.remote.ids = {{ContactIdKind::Account, "account:peer", true},
                               {ContactIdKind::RelayUser, "relay:peer", false}};
    SyncContactMirrors(peer_contact);
    if (!contacts.Upsert(peer_contact)) {
      throw std::runtime_error("Failed to upsert peer contact");
    }

    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = "account:peer";
    target.channel = ThreadChannel::E2e;

    auto created = store.FindOrCreateDirectThread(target, "contact-peer", "Peer");
    if (!created) {
      throw std::runtime_error("Failed to create thread");
    }
    thread = *created;

    if (!identity.LoadOrCreate()) {
      throw std::runtime_error("Failed to load identity");
    }
    {
      LocalIdentity updated = *identity.Get();
      updated.relay_user_id = "relay:local";
      updated.registered = true;
      if (!identity.Update(updated)) {
        throw std::runtime_error("Failed to set test relay id");
      }
    }
    local_relay_id = identity.Get()->relay_user_id;
    local_account_id = identity.Get()->account_id;

    PskSessionRecord psk;
    psk.key = E2eRelayPayloadCodec::ChatTargetFromThread(thread);
    psk.session_epoch = 1;
    const auto master = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    if (!master) {
      throw std::runtime_error("Failed to build test PSK");
    }
    psk.master_psk_b64 = Base64Encode(*master);
    psk.psk_verified_at = 1;
    if (!psk_store.Save(psk)) {
      throw std::runtime_error("Failed to save test PSK");
    }
  }

  RelayEnvelope MakePeerEnvelope(uint64_t seq, const std::string& text) const {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = "peer-msg-" + std::to_string(seq);
    envelope.sender_relay_id = "relay:peer";
    envelope.sender_contact_id = "account:peer";
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2e;

    const auto master = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    if (!master) {
      throw std::runtime_error("Failed to build test PSK");
    }
    E2eEncryptParams params;
    params.text = text;
    params.channel = CryptoChannel::E2e;
    params.peer_contact_id = local_account_id;
    params.sender_contact_id = "account:peer";
    params.message_id = envelope.message_id;
    params.sender_seq = seq;
    params.session_epoch = 1;
    params.timestamp = static_cast<int64_t>(seq);
    auto payload = E2eRelayPayloadCodec::EncryptText(params, *master);
    if (!payload) {
      throw std::runtime_error("Failed to encode encrypted payload");
    }
    envelope.body.e2e.payload_b64 = *payload;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = static_cast<int64_t>(seq);
    envelope.stream_key =
        BuildCanonicalRelayStreamKey(local_relay_id, "relay:peer", ThreadChannel::E2e, envelope.session_epoch);
    auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
    if (!sign_bytes) {
      throw std::runtime_error("Failed to build sign bytes");
    }
    auto signature = MlDsa::Sign(peer_private_key, *sign_bytes);
    if (!signature) {
      throw std::runtime_error("Failed to sign envelope");
    }
    envelope.signature = Base64Encode(*signature);
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
  SqlitePskSessionStore psk_store;
  MockRelayClient relay;
  MockChatHistoryPeerClient peer_history;
  PeerSigningKeyStore key_store;
  PeerSigningKeyResolver key_resolver;
  GroupRosterStore roster_store;
  ContactsStore contacts;
  MockDirectoryClient directory;
  DirectoryShadowCache shadows;
  PeerDisplayResolver labels;
  InboxController inbox;
  RelayReceivePipeline receive_pipeline;
  ChatSyncService sync;
  Thread thread;
  std::string local_relay_id;
  std::string local_account_id;
  MlDsaKeyPair peer_keys;
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

  const RelayReceiveOutcome outcome =
      harness.receive_pipeline.ProcessEnvelope(harness.MakePeerEnvelope(2, "two"), harness.local_account_id);
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
  const RelayReceiveOutcome outcome =
      harness.receive_pipeline.ProcessEnvelope(harness.MakePeerEnvelope(2, "two"), harness.local_account_id);
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
      harness.receive_pipeline.ProcessEnvelope(harness.MakePeerEnvelope(3, "three"), harness.local_account_id);
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

TEST(ChatSyncTest, ScrollBackfillFetchesOlderHistory) {
  SyncTestHarness harness("scroll_backfill");
  harness.SeedPeerSeq(10, "ten");

  PeerSyncState state = DefaultPeerSyncState();
  state.contiguous_peer_seq = 10;
  state.loaded_min_seq = 10;
  state.loaded_max_seq = 10;
  state.history_floor_seq = 4;
  ASSERT_TRUE(static_cast<bool>(harness.store.SetPeerSyncState(harness.thread.id, 1, state)));

  harness.relay.AddDeliveredEnvelope(harness.MakePeerEnvelope(7, "seven"));

  auto result = harness.sync.ScrollBackfill(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_GE(result->ingested, 1u);

  auto messages = harness.store.GetMessages(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(messages));
  bool found_seven = false;
  for (const ThreadMessage& message : *messages) {
    if (message.text == "seven") {
      found_seven = true;
      break;
    }
  }
  EXPECT_TRUE(found_seven);
}

TEST(ChatSyncTest, GapRepairClampsWideSeqSpan) {
  SyncTestHarness harness("gap_clamp");
  harness.SeedPeerSeq(1, "one");

  harness.relay.AddDeliveredEnvelope(harness.MakePeerEnvelope(2, "near"));
  harness.relay.AddDeliveredEnvelope(harness.MakePeerEnvelope(502, "far"));

  auto result = harness.sync.RepairGap(harness.thread.id, 2, 600);
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_GE(result->ingested, 1u);

  auto messages = harness.store.GetMessages(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(messages));
  bool found_near = false;
  bool found_far = false;
  for (const ThreadMessage& message : *messages) {
    if (message.text == "near") {
      found_near = true;
    }
    if (message.text == "far") {
      found_far = true;
    }
  }
  EXPECT_TRUE(found_near);
  EXPECT_FALSE(found_far);
}

TEST(ChatSyncTest, CompromisedThreadBlocksSync) {
  SyncTestHarness harness("compromised_sync");
  harness.SeedPeerSeq(1, "one");

  PeerSyncState compromised = DefaultPeerSyncState();
  compromised.contiguous_peer_seq = 1;
  compromised.loaded_min_seq = 1;
  compromised.loaded_max_seq = 1;
  compromised.phase = PeerSyncPhase::Compromised;
  ASSERT_TRUE(static_cast<bool>(harness.store.SetPeerSyncState(harness.thread.id, 1, compromised)));

  auto tail = harness.sync.TailSync(harness.thread.id);
  ASSERT_FALSE(static_cast<bool>(tail));
  EXPECT_NE(tail.error().message.find("compromised"), std::string::npos);
}

TEST(ChatSyncTest, RelayFetch403ForNonPartyReturnsError) {
  SyncTestHarness harness("relay_403");
  harness.relay.SetFetchHistoryError("Relay history fetch failed with status 403");

  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = harness.local_account_id;
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = "relay:peer";
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.min_sender_seq = 2;
  request.max_sender_seq = 2;
  request.limit = 10;
  request.order = "asc";

  auto response = harness.relay.FetchChatHistory(request);
  ASSERT_FALSE(static_cast<bool>(response));
  EXPECT_NE(response.error().message.find("403"), std::string::npos);
}
