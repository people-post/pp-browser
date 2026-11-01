#include "common/chat/MessagingJson.h"
#include "lib/messaging/EnvelopeSigner.h"
#include "lib/messaging/RelayWirePayload.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "foundation/crypto/CryptoUtil.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

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
  envelope.sender_device = "QmAliceDevice";
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

TEST(P2pRelayWireTest, TheSendersCardRidesWithAKeyInitAndTheDevicesAreSigned) {
  using namespace pbr;
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "550e8400-e29b-41d4-a716-446655440001";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "account:alice";
  envelope.sender_device = "QmAlicePhone";
  envelope.recipient_device = "QmBobLaptop";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2ePublic;
  envelope.body.e2e.payload_b64 = "cGF5bG9hZA==";
  envelope.body.e2e.key_init_b64 = "a2V5X2luaXQ=";
  envelope.timestamp = 1719662400123;
  envelope.signature = "sig";
  const auto unsigned_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  Object card;
  card.set("account_id", "account:alice");
  card.set("card_version", int64_t{2});
  envelope.body.e2e.sender_card = card;
  ASSERT_TRUE(unsigned_bytes);
  EXPECT_EQ(EnvelopeSigner::BuildSignBytes(envelope).value(), *unsigned_bytes)
      << "the card proves itself; the envelope signature does not cover it";
  RelayEnvelope other_device = envelope;
  other_device.recipient_device = "QmBobPhone";
  EXPECT_NE(EnvelopeSigner::BuildSignBytes(other_device).value(), *unsigned_bytes) << "which device it is for is signed";
  RelayEnvelope other_sender = envelope;
  other_sender.sender_device = "QmAliceLaptop";
  EXPECT_NE(EnvelopeSigner::BuildSignBytes(other_sender).value(), *unsigned_bytes) << "and which device sent it";

  const auto parsed = ParseRelayEnvelope(RelayEnvelopeToJson(envelope));
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(parsed.value().sender_device, "QmAlicePhone");
  EXPECT_EQ(parsed.value().recipient_device, "QmBobLaptop");
  ASSERT_TRUE(parsed.value().body.e2e.sender_card);
  EXPECT_EQ(parsed.value().body.e2e.sender_card->getString("account_id"), std::optional<std::string>("account:alice"));

  RelayEnvelope someone_elses = envelope;
  someone_elses.body.e2e.sender_card->set("account_id", "account:mallory");
  EXPECT_FALSE(ParseRelayEnvelope(RelayEnvelopeToJson(someone_elses))) << "a card for another account";
  RelayEnvelope no_key_init = envelope;
  no_key_init.body.e2e.key_init_b64.reset();
  EXPECT_FALSE(ParseRelayEnvelope(RelayEnvelopeToJson(no_key_init)))
      << "a device copy carries the card only with a key_init";
  RelayEnvelope account_copy_no_key_init = no_key_init;
  account_copy_no_key_init.recipient_device.clear();
  EXPECT_TRUE(ParseRelayEnvelope(RelayEnvelopeToJson(account_copy_no_key_init)))
      << "an account copy may always carry it: its reader may know none of our devices";
  RelayEnvelope no_device = envelope;
  no_device.sender_device.clear();
  EXPECT_FALSE(ParseRelayEnvelope(RelayEnvelopeToJson(no_device))) << "every envelope names its sending device";
  RelayEnvelope account_copy = envelope;
  account_copy.recipient_device.clear();
  auto parsed_copy = ParseRelayEnvelope(RelayEnvelopeToJson(account_copy));
  ASSERT_TRUE(parsed_copy);
  EXPECT_TRUE(parsed_copy->recipient_device.empty()) << "an account copy names no recipient device";
}

TEST(P2pRelayWireTest, EveryEnvelopeMaySayHowNewItsSendersListIs) {
  // M021 d6: `sender_card_at` on any copy, card or not; not signed (a false value costs one card ask).
  using namespace pbr;
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "550e8400-e29b-41d4-a716-446655440002";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "account:alice";
  envelope.sender_device = "QmAlicePhone";
  envelope.recipient_device = "QmBobLaptop";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2ePublic;
  envelope.body.e2e.payload_b64 = "cGF5bG9hZA==";
  envelope.timestamp = 1719662400123;
  envelope.signature = "sig";
  const auto unsigned_bytes = EnvelopeSigner::BuildSignBytes(envelope);
  envelope.body.e2e.sender_card_at = 1'761'235'200'000;
  EXPECT_EQ(EnvelopeSigner::BuildSignBytes(envelope).value(), unsigned_bytes.value());
  auto parsed = ParseRelayEnvelope(RelayEnvelopeToJson(envelope));
  ASSERT_TRUE(parsed) << parsed.error().message << " — a device copy with no key_init may say it";
  EXPECT_EQ(parsed.value().body.e2e.sender_card_at, std::optional<int64_t>(1'761'235'200'000));
  envelope.body.e2e.sender_card_at.reset();
  auto without = ParseRelayEnvelope(RelayEnvelopeToJson(envelope));
  ASSERT_TRUE(without);
  EXPECT_FALSE(without.value().body.e2e.sender_card_at) << "optional";
}

TEST(P2pRelayWireTest, RejectsUnsafeMessageId) {
  using namespace pbr;

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = R"(x');evil('&quot; onmouseover=&quot;alert(1))";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "relay:alice123";
  envelope.sender_device = "QmAliceDevice";
  envelope.route.kind = "direct";
  envelope.route.channel = ThreadChannel::E2e;
  envelope.body.e2e.payload_b64 = "ignored";
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope.timestamp = 1719662400123;
  envelope.signature = "sig";

  // message_id ends up unescaped in generated RML (open_attachment('<id>')); an id carrying
  // quotes/HTML must be rejected at parse time rather than relying on downstream escaping.
  EXPECT_FALSE(static_cast<bool>(ParseRelayEnvelope(RelayEnvelopeToJson(envelope))));
}

TEST(P2pRelayWireTest, ChatHistoryResponseSkipsOneBadMessageInsteadOfFailingThePage) {
  using namespace pbr;

  RelayEnvelope good;
  good.envelope_version = kRelayEnvelopeVersion;
  good.message_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  good.sender_relay_id = "relay:alice123";
  good.sender_contact_id = "relay:alice123";
  good.sender_device = "QmAliceDevice";
  good.route.kind = "direct";
  good.route.channel = ThreadChannel::E2e;
  good.body.e2e.payload_b64 = "ignored";
  good.sender_seq = 1;
  good.session_epoch = 1;
  good.timestamp = 1719662400123;
  good.signature = "sig";

  Object bad = RelayEnvelopeToJson(good);
  bad.set("message_id", R"(bad"id)");

  Object response;
  response.set("peer_identity_kind", "relay_user");
  response.set("peer_identity_value", "relay:alice123");
  response.set("has_more", false);
  response.set("messages", makeArray(std::vector<Value>{
                              std::make_shared<Object>(RelayEnvelopeToJson(good)),
                              std::make_shared<Object>(bad),
                          }));

  auto parsed = ChatHistoryResponseFromJson(response);
  ASSERT_TRUE(static_cast<bool>(parsed)) << parsed.error().message;
  ASSERT_EQ(parsed->messages.size(), 1u);
  EXPECT_EQ(parsed->messages[0].message_id, good.message_id);
}

TEST(P2pRelayWireTest, RelayWireRecordRoundTrip) {
  using namespace pbr;

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "550e8400-e29b-41d4-a716-446655440000";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "relay:alice123";
  envelope.sender_device = "QmAliceDevice";
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

TEST(P2pRelayWireTest, RelayWireRecordUsesSenderRelayIdForHttpAuth) {
  using namespace pbr;

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "550e8400-e29b-41d4-a716-446655440000";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "account:aliceAccountId";
  envelope.sender_device = "QmAliceDevice";
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
  EXPECT_EQ(wire.value().sender_contact_id, "relay:alice123");
  EXPECT_EQ(wire.value().recipient_contact_id, "relay:bob456");

  auto blob = Base64Decode(wire.value().blob_b64);
  ASSERT_TRUE(static_cast<bool>(blob));
  const std::string serialized(blob->begin(), blob->end());
  auto app = TryParseObject(serialized);
  ASSERT_TRUE(app.has_value());
  EXPECT_EQ(app->getString("sender_contact_id"), std::optional<std::string>("account:aliceAccountId"));
  EXPECT_EQ(app->getString("sender_relay_id"), std::optional<std::string>("relay:alice123"));
}

TEST(P2pRelayWireTest, UrgentWakeIsARelayHintOutsideTheEnvelope) {
  using namespace pbr;

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "550e8400-e29b-41d4-a716-446655440000";
  envelope.sender_relay_id = "relay:alice123";
  envelope.sender_contact_id = "relay:alice123";
  envelope.sender_device = "QmAliceDevice";
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

  const auto plain = RelayWireSendRecordFromEnvelope(envelope);
  ASSERT_TRUE(static_cast<bool>(plain));
  EXPECT_FALSE(plain.value().urgent);
  EXPECT_FALSE(RelayWireSendRecordToJson(plain.value()).contains("urgent"));

  envelope.relay_wake_urgent = true;
  const auto urgent = RelayWireSendRecordFromEnvelope(envelope);
  ASSERT_TRUE(static_cast<bool>(urgent));
  EXPECT_TRUE(urgent.value().urgent);
  EXPECT_EQ(RelayWireSendRecordToJson(urgent.value()).getIf<bool>("urgent"), std::optional<bool>(true));
  // The hint is not part of what the recipient gets.
  EXPECT_EQ(urgent.value().blob_b64, plain.value().blob_b64);
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
