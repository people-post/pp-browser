#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

TEST(P2pRelayWireTest, RejectsLegacyThreadIdEnvelope) {
  using namespace pbr;

  const nlohmann::json legacy = {{"thread_id", "t1"},
                                 {"message_id", "m1"},
                                 {"sender_relay_id", "relay:a"},
                                 {"body", {{"text", "hi"}}},
                                 {"timestamp", 1}};
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

TEST(P2pRelayWireTest, DirectTargetRoutingAndOutboxReconcile) {
  using namespace pbr;

  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_p2p_routing_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());

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
}
