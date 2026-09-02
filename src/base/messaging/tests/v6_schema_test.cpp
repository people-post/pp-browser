#include "common/chat/MessagingJson.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/messaging/SyncStateCodec.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

} // namespace
} // namespace pbr

TEST(V6SchemaTest, OutboundMessagePersistsSenderSeqAndEpoch) {
  using namespace pbr;

  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_v6_outbound_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.SetDek(TestDek()));

  DirectChatTarget target;
  target.peer_identity_kind = "relay_user";
  target.peer_identity_value = "relay:alice";
  target.channel = ThreadChannel::E2e;

  auto thread = store.FindOrCreateDirectThread(target, "contact-alice", "Alice");
  ASSERT_TRUE(static_cast<bool>(thread));

  auto seq = store.AllocateSenderSeq(thread->id);
  ASSERT_TRUE(static_cast<bool>(seq));
  auto epoch = store.GetChatTargetSessionEpoch(thread->id);
  ASSERT_TRUE(static_cast<bool>(epoch));
  EXPECT_EQ(*epoch, 1u);

  ThreadMessage message;
  message.id = "msg-outbound";
  message.thread_id = thread->id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = "seq hello";
  message.timestamp = 1;
  message.delivery = MessageDelivery::Pending;
  message.relay_visible = true;
  message.transport = MessageTransport::Relay;
  message.sender_seq = *seq;
  message.session_epoch = *epoch;
  ASSERT_TRUE(static_cast<bool>(store.AppendMessage(message)));

  auto loaded = store.GetMessages(thread->id);
  ASSERT_TRUE(static_cast<bool>(loaded));
  ASSERT_EQ(loaded->size(), 1u);
  ASSERT_TRUE(loaded->front().sender_seq.has_value());
  ASSERT_TRUE(loaded->front().session_epoch.has_value());
  EXPECT_EQ(*loaded->front().sender_seq, *seq);
  EXPECT_EQ(*loaded->front().session_epoch, *epoch);
}

TEST(V6SchemaTest, GetMessagesBySeqRangeFiltersPeerStream) {
  using namespace pbr;

  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_v6_seq_range_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.SetDek(TestDek()));

  DirectChatTarget target;
  target.peer_identity_kind = "relay_user";
  target.peer_identity_value = "relay:bob";
  target.channel = ThreadChannel::E2e;

  auto thread = store.FindOrCreateDirectThread(target, "contact-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(thread));

  auto append_seq = [&](uint64_t seq, const std::string& sender, const std::string& text) {
    ThreadMessage message;
    message.id = "msg-" + std::to_string(seq) + sender;
    message.thread_id = thread->id;
    message.sender_contact_id = sender;
    message.text = text;
    message.timestamp = static_cast<int64_t>(seq);
    message.delivery = MessageDelivery::Relayed;
    message.relay_visible = true;
    message.transport = MessageTransport::Relay;
    message.sender_seq = seq;
    message.session_epoch = 1;
    return store.AppendMessage(message);
  };

  ASSERT_TRUE(static_cast<bool>(append_seq(1, kLocalSelfContactId, "local-1")));
  ASSERT_TRUE(static_cast<bool>(append_seq(2, "contact-bob", "peer-2")));
  ASSERT_TRUE(static_cast<bool>(append_seq(3, "contact-bob", "peer-3")));

  SeqRangeQuery query;
  query.session_epoch = 1;
  query.seq_owner_contact_id = "contact-bob";
  query.min_sender_seq = 2;
  query.max_sender_seq = 3;
  query.limit = 10;

  auto peer_rows = store.GetMessagesBySeqRange(thread->id, query);
  ASSERT_TRUE(static_cast<bool>(peer_rows));
  ASSERT_EQ(peer_rows->size(), 2u);
  EXPECT_EQ((*peer_rows)[0].text, "peer-2");
  EXPECT_EQ((*peer_rows)[1].text, "peer-3");
}

TEST(V6SchemaTest, SyncStateInitializedForE2eDirectThread) {
  using namespace pbr;

  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_v6_sync_state_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.SetDek(TestDek()));

  DirectChatTarget target;
  target.peer_identity_kind = "relay_user";
  target.peer_identity_value = "relay:carol";
  target.channel = ThreadChannel::E2e;

  auto thread = store.FindOrCreateDirectThread(target, "contact-carol", "Carol");
  ASSERT_TRUE(static_cast<bool>(thread));

  auto state = store.GetPeerSyncState(thread->id, 1);
  ASSERT_TRUE(static_cast<bool>(state));
  EXPECT_EQ(state->phase, PeerSyncPhase::Ok);
  EXPECT_EQ(state->contiguous_peer_seq, 0u);

  PeerSyncState updated = *state;
  updated.contiguous_peer_seq = 4;
  updated.loaded_max_seq = 4;
  ASSERT_TRUE(static_cast<bool>(store.SetPeerSyncState(thread->id, 1, updated)));

  auto reloaded = store.GetPeerSyncState(thread->id, 1);
  ASSERT_TRUE(static_cast<bool>(reloaded));
  EXPECT_EQ(reloaded->contiguous_peer_seq, 4u);
  EXPECT_EQ(reloaded->loaded_max_seq, 4u);
}

TEST(V6SchemaTest, RelayEnvelopeRoundTripPreservesSeqFields) {
  using namespace pbr;

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "msg-seq";
  envelope.sender_relay_id = "relay:peer";
  envelope.sender_contact_id = "relay:peer";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.body.e2e.payload_b64 = "aGk=";
  envelope.sender_seq = 7;
  envelope.session_epoch = 2;
  envelope.timestamp = 99;

  auto parsed = ParseRelayEnvelope(RelayEnvelopeToJson(envelope));
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed->sender_seq, 7u);
  EXPECT_EQ(parsed->session_epoch, 2u);
}
