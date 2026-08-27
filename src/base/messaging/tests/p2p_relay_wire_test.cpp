#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/ValueJson.h"

namespace {
using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}
} // namespace

TEST(P2pRelayWireTest, RejectsLegacyThreadIdEnvelope) {
  using namespace pbr;

  const Object legacy = TryParseObject(R"({
    "thread_id": "t1",
    "message_id": "m1",
    "sender_relay_id": "relay:a",
    "body": {"text": "hi"},
    "timestamp": 1
  })").value_or(Object{});
  EXPECT_FALSE(static_cast<bool>(ParseRelayEnvelope(legacy)));
}

TEST(P2pRelayWireTest, RelayEnvelopeRoundTripAndPayloadCodec) {
  using namespace pbr;

  auto payload_b64 = RelayWirePayload::EncodePlaintextText("hello relay");
  ASSERT_TRUE(static_cast<bool>(payload_b64));

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "550e8400-e29b-41d4-a716-446655440000";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "relay:alice123";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.body.e2e.payload_b64 = *payload_b64;
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope.timestamp = 1719662400123;
  envelope.signature = "sig";

  const auto parsed = ParseRelayEnvelope(RelayEnvelopeToJson(envelope));
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed.value().message_id, envelope.message_id);
  EXPECT_EQ(parsed.value().route.channel, ThreadChannel::E2e);

  auto decoded = RelayWirePayload::DecodePlaintextText(parsed.value().body.e2e.payload_b64);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(*decoded, "hello relay");
}

TEST(P2pRelayWireTest, RelayWireRecordRoundTrip) {
  using namespace pbr;

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "550e8400-e29b-41d4-a716-446655440000";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "relay:alice123";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.body.e2e.payload_b64 = "AA==";
  envelope.sender_seq = 7;
  envelope.order_key = 7;
  envelope.session_epoch = 1;
  envelope.timestamp = 1719662400123;
  envelope.signature = "sig";
  envelope.stream_key = "v1:e2e:1:relay:alice123:relay:bob456";
  envelope.recipient_contact_id = "relay:bob456";

  const auto wire = RelayWireSendRecordFromEnvelope(envelope);
  ASSERT_TRUE(static_cast<bool>(wire));
  EXPECT_EQ(wire.value().stream_id, envelope.stream_key);
  EXPECT_EQ(wire.value().index_key, 7u);

  RelayInboundRecord inbound;
  inbound.sender_contact_id = wire.value().sender_contact_id;
  inbound.stream_id = wire.value().stream_id;
  inbound.index_key = wire.value().index_key;
  inbound.blob_b64 = wire.value().blob_b64;

  const auto restored = RelayEnvelopeFromInboundRecord(inbound);
  ASSERT_TRUE(static_cast<bool>(restored));
  EXPECT_EQ(restored.value().message_id, envelope.message_id);
  EXPECT_EQ(restored.value().stream_key, envelope.stream_key);
  EXPECT_EQ(restored.value().order_key, envelope.order_key);
  EXPECT_FALSE(restored.value().recipient_contact_id.has_value());
}

TEST(P2pRelayWireTest, DirectTargetRoutingAndOutboxReconcile) {
  using namespace pbr;

  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_p2p_routing_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.SetDek(TestDek()));

  DirectChatTarget target;
  target.peer_identity_kind = "relay_user";
  target.peer_identity_value = "relay:bob456";
  target.channel = ThreadChannel::E2e;

  auto created = store.FindOrCreateDirectThread(target, "contact-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(created));
  EXPECT_EQ(created->channel, ThreadChannel::E2e);
  EXPECT_TRUE(created->encrypted);

  auto same = store.FindOrCreateDirectThread(target, "contact-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(same));
  EXPECT_EQ(same->id, created->id);

  DirectChatTarget public_target = target;
  public_target.channel = ThreadChannel::E2ePublic;
  auto public_thread = store.FindOrCreateDirectThread(public_target, "contact-bob", "Bob");
  ASSERT_TRUE(static_cast<bool>(public_thread));
  EXPECT_NE(public_thread->id, created->id);
  EXPECT_EQ(public_thread->channel, ThreadChannel::E2ePublic);

  ThreadMessage pending;
  pending.id = "pending-msg";
  pending.thread_id = created->id;
  pending.sender_contact_id = kLocalSelfContactId;
  pending.text = "queued";
  pending.timestamp = 1;
  pending.delivery = MessageDelivery::Pending;
  pending.relay_visible = true;
  ASSERT_TRUE(static_cast<bool>(store.AppendMessage(pending)));

  auto outbox = store.ListPendingOutbox();
  ASSERT_TRUE(static_cast<bool>(outbox));
  ASSERT_EQ(outbox->size(), 1u);
  EXPECT_EQ(outbox->front().first, "pending-msg");

  pending.delivery = MessageDelivery::Relayed;
  ASSERT_TRUE(static_cast<bool>(store.UpdateMessage(pending)));

  ASSERT_TRUE(static_cast<bool>(store.ReconcileOutbox()));
  outbox = store.ListPendingOutbox();
  ASSERT_TRUE(static_cast<bool>(outbox));
  EXPECT_TRUE(outbox->empty());

  ASSERT_TRUE(static_cast<bool>(store.ClearMessages(created->id, {})));
  outbox = store.ListPendingOutbox();
  ASSERT_TRUE(static_cast<bool>(outbox));
  EXPECT_TRUE(outbox->empty());

  auto deleted = store.DeleteThread(created->id);
  ASSERT_TRUE(static_cast<bool>(deleted));
  auto gone = store.GetThread(created->id);
  ASSERT_TRUE(static_cast<bool>(gone));
  EXPECT_FALSE(gone->has_value());
}
