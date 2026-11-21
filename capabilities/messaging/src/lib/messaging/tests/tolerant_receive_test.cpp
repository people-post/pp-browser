// Readers that tolerate what they do not know (release-compatibility INVENTORY T1, T3, T5, U1, U2): a newer app's
// message is kept as "update to view" with its seq consumed, a refusal after a good verify consumes its seq, and a
// newer account card marks its sender instead of being asked for again and again.
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/EncryptedPayload.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/E2eRelayPayloadCodec.h"
#include "lib/delivery/PskRotateCodec.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/messaging/tests/TestAccount.h"
#include "common/chat/MessagingJson.h"
#include "common/chat/MessagingLimits.h"
#include "common/Serialize.hpp"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr const char* kPeer = "account:peer";

ByteVector TestMasterPsk() {
  return *HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

/** A ChatPayload as a newer app might write it: version byte `version`, content type `type`, a text. */
std::vector<uint8_t> PayloadBytes(uint8_t version, uint8_t type, const std::string& text) {
  std::ostringstream oss;
  {
    OutputArchive ar(oss);
    ar & version;
    ar & type;
    WireLenUtf8 field{text};
    ar & field;
  }
  const std::string data = oss.str();
  return std::vector<uint8_t>(data.begin(), data.end());
}

class TolerantReceiveTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(store_.SetDek(TestDek()));
    ASSERT_TRUE(psk_store_.SetDek(TestDek()));
    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = kPeer;
    target.channel = ThreadChannel::E2e;
    auto thread = store_.FindOrCreateDirectThread(target, "contact-peer", "Peer");
    ASSERT_TRUE(thread);
    thread_ = *thread;
    PeerSessionRecord record;
    record.key = E2eRelayPayloadCodec::ChatTargetFromThread(thread_);
    record.session_epoch = 1;
    record.master_psk_b64 = Base64Encode(TestMasterPsk());
    record.psk_verified_at = 1;
    ASSERT_TRUE(psk_store_.Save(record));
    pipeline_ = std::make_unique<RelayReceivePipeline>(
        store_,
        [card = peer_.DeviceCard()](const std::string& account) -> std::optional<AccountCard> {
          return account == kPeer ? std::optional<AccountCard>(card) : std::nullopt;
        },
        psk_store_, test::ReceiveKeysOf(local_), roster_store_);
  }

  /** The peer's envelope at `seq` carrying `plaintext` (a ChatPayload), encrypted, then `tamper`ed, then signed. */
  RelayEnvelope Envelope(const uint64_t seq, const std::vector<uint8_t>& plaintext,
                         const std::function<void(ByteVector&)>& tamper = nullptr) {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = "peer-msg-" + std::to_string(seq);
    envelope.sender_contact_id = kPeer;
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2e;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = static_cast<int64_t>(1000 + seq);
    E2eEncryptParams params;
    params.channel = CryptoChannel::E2e;
    params.peer_contact_id = local_.account_id;
    params.sender_contact_id = kPeer;
    params.message_id = envelope.message_id;
    params.sender_seq = seq;
    params.session_epoch = 1;
    params.timestamp = envelope.timestamp;
    auto payload_b64 = E2eRelayPayloadCodec::EncryptChatPayloadBytes(params, plaintext, TestMasterPsk());
    EXPECT_TRUE(payload_b64);
    envelope.body.e2e.payload_b64 = *payload_b64;
    if (tamper) {
      auto blob = Base64Decode(envelope.body.e2e.payload_b64);
      EXPECT_TRUE(blob);
      tamper(*blob);
      envelope.body.e2e.payload_b64 = Base64Encode(*blob);
    }
    test::SignEnvelopeAs(peer_, envelope);
    return envelope;
  }

  RelayEnvelope TextEnvelope(const uint64_t seq, const std::string& text) {
    return Envelope(seq, *ChatPayloadCodec::EncodeText(text));
  }

  PeerSyncState Sync() { return store_.GetPeerSyncState(thread_.id, 1).value(); }

  std::vector<ThreadMessage> Rows() { return store_.GetMessages(thread_.id).value(); }

  static bool Closed(const PeerSyncState& state, const uint64_t seq) {
    return std::find(state.empty_closed_seqs.begin(), state.empty_closed_seqs.end(), seq) !=
           state.empty_closed_seqs.end();
  }

  InMemoryDeliveryStore store_;
  InMemoryPeerSessions psk_store_{store_};
  InMemoryGroupRosterStore roster_store_{store_};
  test::TestAccount local_ = test::TestAccount::Make();
  test::TestAccount peer_ = test::TestAccount::Make();
  Thread thread_;
  std::unique_ptr<RelayReceivePipeline> pipeline_;
};

// T1: an unknown content type is stored as received and shown as "update to view"; its seq is consumed, so the next
// message is contiguous.
TEST_F(TolerantReceiveTest, AnUnknownContentTypeIsKeptAsUpdateToViewAndItsSeqConsumed) {
  const std::vector<uint8_t> newer = PayloadBytes(1, 42, "a poll");
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(Envelope(1, newer), local_.account_id);
  EXPECT_TRUE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  EXPECT_EQ(outcome.receive_failure_sender.value_or(""), kPeer);
  EXPECT_FALSE(outcome.receive_failure_thread_id) << "no failure line: the placeholder row is the line";
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].content_type, ChatContentType::Unsupported);
  auto row = ChatPayloadCodec::EncodeToRow(rows[0]);
  ASSERT_TRUE(row);
  EXPECT_EQ(*row, newer) << "stored as received, for a newer app to read";
  EXPECT_EQ(Sync().contiguous_peer_seq, 1u);

  const RelayReceiveOutcome next = pipeline_->ProcessEnvelope(TextEnvelope(2, "hello"), local_.account_id);
  EXPECT_EQ(next.decision, IngestDecision::AcceptContiguous);
  EXPECT_FALSE(next.receive_failure);
}

// T3: a higher payload_version after a good decrypt is "update needed", not "couldn't decrypt", and is kept.
TEST_F(TolerantReceiveTest, AHigherPayloadVersionIsUpdateNeededNotDecryptFailed) {
  const RelayReceiveOutcome outcome =
      pipeline_->ProcessEnvelope(Envelope(1, PayloadBytes(2, 0, "v2 layout")), local_.account_id);
  EXPECT_TRUE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].content_type, ChatContentType::Unsupported);
  EXPECT_TRUE(rows[0].text.empty()) << "nothing after a newer version byte is read";
  EXPECT_EQ(Sync().contiguous_peer_seq, 1u);
}

// T3: an unknown sub_version of a known type is the same.
TEST_F(TolerantReceiveTest, AHigherSubVersionIsUpdateNeeded) {
  std::vector<uint8_t> bytes = PayloadBytes(1, 5, ""); // attachment
  bytes.push_back(9);                                  // its sub_version, newer than 1
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(Envelope(1, bytes), local_.account_id);
  EXPECT_TRUE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  EXPECT_EQ(Rows().at(0).content_type, ChatContentType::Unsupported);
}

// T3: a higher blob byte (signed, so not a forgery) is stood in for; crypto stays strict for anything else.
TEST_F(TolerantReceiveTest, AHigherBlobByteIsStoodInForAndItsSeqConsumed) {
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(
      Envelope(1, *ChatPayloadCodec::EncodeText("x"), [](ByteVector& blob) { blob[0] = kEncryptedPayloadVersion + 1; }),
      local_.account_id);
  EXPECT_TRUE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].content_type, ChatContentType::Unsupported);
  EXPECT_TRUE(ChatPayloadCodec::EncodeToRow(rows[0])) << "the stand-in is storable";
  EXPECT_EQ(Sync().contiguous_peer_seq, 1u);
}

TEST_F(TolerantReceiveTest, ABrokenCiphertextIsStillDecryptFailedAndNotKept) {
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(
      Envelope(1, *ChatPayloadCodec::EncodeText("x"), [](ByteVector& blob) { blob.back() ^= 0x01; }),
      local_.account_id);
  EXPECT_FALSE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::DecryptFailed);
  EXPECT_TRUE(Rows().empty());
}

// T5: a reaction over the cap is refused after a good verify, and its seq is consumed — in a private chat a resend
// is not read as the seq going back (no compromise latch).
TEST_F(TolerantReceiveTest, AReactionOverTheCapIsRefusedAndItsSeqConsumed) {
  ASSERT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(1, "target"), local_.account_id).decision,
            IngestDecision::AcceptBootstrap);
  for (size_t i = 0; i < kMaxAnnotationsPerTarget; ++i) {
    ThreadMessage reaction;
    reaction.id = "local-reaction-" + std::to_string(i);
    reaction.thread_id = thread_.id;
    reaction.sender_contact_id = kLocalSelfContactId;
    reaction.content_type = ChatContentType::Annotation;
    reaction.target_message_id = "peer-msg-1";
    reaction.payload_json = R"({"annotation_type":"reaction","target_message_id":"peer-msg-1","value":"+1"})";
    reaction.delivery = MessageDelivery::Local;
    ASSERT_TRUE(store_.AppendMessage(reaction));
  }
  ChatAnnotationFields fields{
      .text = "", .annotation_type = kAnnotationTypeReaction, .target_message_id = "peer-msg-1", .value = "+1"};
  const RelayEnvelope over = Envelope(2, *ChatPayloadCodec::EncodeAnnotation(fields));
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(over, local_.account_id);
  EXPECT_FALSE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::ReactionRejected);
  EXPECT_EQ(Sync().contiguous_peer_seq, 2u) << "consumed";
  EXPECT_TRUE(Closed(Sync(), 2));

  const RelayReceiveOutcome again = pipeline_->ProcessEnvelope(over, local_.account_id);
  EXPECT_NE(again.decision, IngestDecision::SoftCompromised);
  EXPECT_NE(Sync().phase, PeerSyncPhase::Compromised);
  EXPECT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(3, "next"), local_.account_id).decision,
            IngestDecision::AcceptContiguous);
}

// T5 + U1: a psk_rotate with a wrap a newer app made is "update needed", and its seq is consumed.
TEST_F(TolerantReceiveTest, ARotateWithAnUnknownWrapIsUpdateNeededAndItsSeqConsumed) {
  ThreadMessage rotate;
  rotate.content_type = ChatContentType::System;
  Object detail;
  detail.set("rotation_id", "r1");
  detail.set("new_epoch", static_cast<int64_t>(2));
  detail.set("wrap_kind", "future_kem");
  detail.set("thread_kem_pk_b64", "AAAA");
  detail.set("key_init_hash", std::string(64, 'a'));
  Object payload;
  payload.set("control_type", kPskRotateControlType);
  payload.set("detail", DumpJson(detail));
  rotate.payload_json = DumpJson(payload);
  const RelayReceiveOutcome outcome =
      pipeline_->ProcessEnvelope(Envelope(1, *ChatPayloadCodec::EncodeToRow(rotate)), local_.account_id);
  EXPECT_FALSE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  EXPECT_EQ(Sync().contiguous_peer_seq, 1u);
  EXPECT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(2, "next"), local_.account_id).decision,
            IngestDecision::AcceptContiguous);
}

// U2: a card of a newer card_version riding an envelope tells the caller, before any ask for the card.
TEST_F(TolerantReceiveTest, ANewerCardRidingAnEnvelopeMarksItsSender) {
  std::vector<std::string> unreadable;
  std::vector<std::string> asked;
  pipeline_->SetOnUnreadableSenderCard([&](const std::string& account) { unreadable.push_back(account); });
  pipeline_->SetOnUnknownSenderDevice([&](const std::string& account, const std::string&, int64_t) {
    EXPECT_EQ(unreadable.size(), 1u) << "marked before the ask, so the caller can stop it";
    asked.push_back(account);
  });
  RelayEnvelope envelope = TextEnvelope(1, "hi");
  Object card = AccountCardToJson(peer_.DeviceCard());
  card.set("card_version", static_cast<int64_t>(6));
  card.set("account_id", kPeer);
  envelope.body.e2e.sender_card = card;
  envelope.body.e2e.sender_card_at = 1;
  envelope.sender_device = "device:not-on-the-kept-card";
  (void)pipeline_->ProcessEnvelope(envelope, local_.account_id);
  ASSERT_EQ(unreadable.size(), 1u);
  EXPECT_EQ(unreadable[0], kPeer);
}

TEST_F(TolerantReceiveTest, ACardOfThisVersionIsNotMarked) {
  bool marked = false;
  pipeline_->SetOnUnreadableSenderCard([&](const std::string&) { marked = true; });
  RelayEnvelope envelope = TextEnvelope(1, "hi");
  envelope.body.e2e.sender_card = AccountCardToJson(peer_.DeviceCard());
  (void)pipeline_->ProcessEnvelope(envelope, local_.account_id);
  EXPECT_FALSE(marked);
}

// The codec below the pipeline: what a newer app wrote reads as Unsupported, kept whole; what is broken stays an error.
TEST(TolerantCodecTest, AnUnknownTypeRoundTripsItsBytesThroughTheRow) {
  const std::vector<uint8_t> newer = PayloadBytes(1, 42, "a poll");
  auto message = ChatPayloadCodec::DecodeToMessageFields(newer);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->content_type, ChatContentType::Unsupported);
  auto row = ChatPayloadCodec::EncodeToRow(*message);
  ASSERT_TRUE(row);
  EXPECT_EQ(*row, newer);
  auto again = ChatPayloadCodec::DecodeToMessageFields(*row);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->content_type, ChatContentType::Unsupported);
  const auto facts = TryParseObject(again->payload_json);
  ASSERT_TRUE(facts);
  EXPECT_EQ(facts->getIf<int64_t>("wire_content_type").value_or(0), 42);
}

TEST(TolerantCodecTest, NewerTextSpansAreUnsupported) {
  std::vector<uint8_t> bytes = PayloadBytes(1, 6, "hi @bob");
  bytes.push_back(2); // text_spans sub_version 2
  auto message = ChatPayloadCodec::DecodeToMessageFields(bytes);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->content_type, ChatContentType::Unsupported);
}

TEST(TolerantCodecTest, BrokenOrOlderPayloadsAreStillRefused) {
  EXPECT_FALSE(ChatPayloadCodec::DecodeToMessageFields(PayloadBytes(0, 0, "x"))) << "version 0 is no version";
  std::vector<uint8_t> retired = PayloadBytes(1, 3, "");
  retired.push_back(2); // contact card sub_version 2: retired, not newer
  EXPECT_FALSE(ChatPayloadCodec::DecodeToMessageFields(retired));
  std::vector<uint8_t> trailing = *ChatPayloadCodec::EncodeText("x");
  trailing.push_back(0); // a known version with bytes after its end (T4 is other work)
  EXPECT_FALSE(ChatPayloadCodec::DecodeToMessageFields(trailing));
}

TEST(TolerantCodecTest, AStandInWithNothingKeptIsStorableAndReadsBackUnsupported) {
  ThreadMessage placeholder;
  placeholder.content_type = ChatContentType::Unsupported;
  placeholder.payload_json = R"({"needs_update":"encrypted payload version 2"})";
  auto row = ChatPayloadCodec::EncodeToRow(placeholder);
  ASSERT_TRUE(row);
  auto back = ChatPayloadCodec::DecodeToMessageFields(*row);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->content_type, ChatContentType::Unsupported);
}

TEST(TolerantCodecTest, ANewerBlobByteAndGroupCopyVersionAreNewerFormatNotBroken) {
  ByteVector blob(1 + kAeadNonceSize + 32, 0);
  blob[0] = kEncryptedPayloadVersion + 1;
  auto decoded = EncryptedPayload::DecodeBlob(blob);
  ASSERT_FALSE(decoded);
  EXPECT_TRUE(AppError::IsNewerFormat(decoded.error()));
  blob[0] = 0;
  auto broken = EncryptedPayload::DecodeBlob(blob);
  ASSERT_FALSE(broken);
  EXPECT_FALSE(AppError::IsNewerFormat(broken.error()));

  auto copy = GroupE2ePayloadCodec::EncodeCopyPlaintext("group-1", *ChatPayloadCodec::EncodeText("x"),
                                                        GroupCopyState{.statement_epoch = 1, .changes_hash = ""});
  ASSERT_TRUE(copy) << copy.error().message;
  (*copy)[0] = static_cast<uint8_t>((*copy)[0] + 1);
  auto read = GroupE2ePayloadCodec::DecodeCopyPlaintext(*copy);
  ASSERT_FALSE(read);
  EXPECT_TRUE(AppError::IsNewerFormat(read.error()));
}

// ---- T2: a higher envelope_version -----------------------------------------------------------------------------

/** `envelope` as a newer app would send it: a higher version and a body this app cannot read. */
Object NewerEnvelopeJson(const RelayEnvelope& envelope, const int64_t version = kRelayEnvelopeVersion + 1) {
  Object json = RelayEnvelopeToJson(envelope);
  json.set("envelope_version", version);
  Object body = *json.getObject("body");
  Object e2e = *body.getObject("e2e");
  Object copy = *asObject(e2e.getArray("copies")->elements.front());
  copy.erase("payload_b64");
  copy.set("sealed_v4", "AQID"); // a newer field this app does not know
  e2e.set("copies", ArrayValue({ObjectValue(copy)}));
  body.set("e2e", e2e);
  json.set("body", body);
  return json;
}

TEST(TolerantEnvelopeParseTest, AHigherVersionIsReadForItsRoutingAndNumberingOnly) {
  RelayEnvelope sent;
  sent.message_id = "newer-1";
  sent.sender_contact_id = kPeer;
  sent.sender_device = "QmPeerDevice";
  sent.route.kind = kRelayRouteDirect;
  sent.route.channel = ThreadChannel::E2ePublic;
  sent.timestamp = 77;
  sent.body.e2e.copies = {RelayCopy{.recipient_device = "", .session_epoch = 2, .sender_seq = 9, .payload_b64 = "AQ"}};
  auto parsed = ParseRelayEnvelope(NewerEnvelopeJson(sent));
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(parsed->envelope_version, kRelayEnvelopeVersion + 1) << "kept as sent: the pipeline routes on it";
  EXPECT_EQ(parsed->message_id, "newer-1");
  EXPECT_EQ(parsed->session_epoch, 2u);
  EXPECT_EQ(parsed->sender_seq, 9u);
  EXPECT_EQ(parsed->route.channel, ThreadChannel::E2ePublic);
  EXPECT_TRUE(parsed->body.e2e.payload_b64.empty()) << "nothing of the body is read";

  Object no_numbering = NewerEnvelopeJson(sent);
  no_numbering.erase("body");
  EXPECT_FALSE(ParseRelayEnvelope(no_numbering)) << "nothing to number: refused as before";
  EXPECT_FALSE(ParseRelayEnvelope(NewerEnvelopeJson(sent, kRelayEnvelopeVersion - 1))) << "older: still refused";
  EXPECT_FALSE(ParseRelayEnvelope(NewerEnvelopeJson(sent, (int64_t{1} << 32) + kRelayEnvelopeVersion)))
      << "no version past int32";
}

class NewerEnvelopeTest : public TolerantReceiveTest {
protected:
  /** The peer's message at `seq` as a newer app sends it (its copy for our account). */
  RelayEnvelope Newer(const uint64_t seq) {
    RelayEnvelope envelope = TextEnvelope(seq, "unread");
    envelope.message_id = "newer-msg-" + std::to_string(seq);
    auto parsed = ParseRelayEnvelope(NewerEnvelopeJson(envelope));
    EXPECT_TRUE(parsed) << parsed.error().message;
    return *parsed;
  }
};

TEST_F(NewerEnvelopeTest, ItsSeqIsClosedAndItShowsAsUpdateToView) {
  ASSERT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(1, "one"), local_.account_id).decision,
            IngestDecision::AcceptBootstrap);
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(Newer(2), local_.account_id);
  EXPECT_TRUE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  EXPECT_FALSE(outcome.sender_verified) << "nothing of it is verified, so nothing of it is applied";
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 2u);
  const ThreadMessage& shown = rows[0].id == "newer-msg-2" ? rows[0] : rows[1];
  EXPECT_EQ(shown.content_type, ChatContentType::Unsupported);
  EXPECT_FALSE(shown.sender_seq) << "it holds no seq: a genuine message there would still be taken";
  EXPECT_EQ(Sync().contiguous_peer_seq, 2u);
  EXPECT_TRUE(Closed(Sync(), 2));
  EXPECT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(3, "three"), local_.account_id).decision,
            IngestDecision::AcceptContiguous)
      << "no gap behind it, so no repair";

  EXPECT_EQ(pipeline_->ProcessEnvelope(Newer(2), local_.account_id).decision, IngestDecision::BenignDuplicate);
  EXPECT_EQ(Rows().size(), 3u) << "one row, however often it comes (poll, history, link)";
}

// The stall INVENTORY § 2.1 names: later messages came first and the thread sat in Gap, repairing for good.
TEST_F(NewerEnvelopeTest, ArrivingAfterLaterMessagesItTakesTheThreadOutOfGap) {
  ASSERT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(1, "one"), local_.account_id).decision,
            IngestDecision::AcceptBootstrap);
  ASSERT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(3, "three"), local_.account_id).decision,
            IngestDecision::AcceptGap);
  ASSERT_EQ(Sync().phase, PeerSyncPhase::Gap);
  (void)pipeline_->ProcessEnvelope(Newer(2), local_.account_id);
  EXPECT_EQ(Sync().contiguous_peer_seq, 3u);
  EXPECT_EQ(Sync().phase, PeerSyncPhase::Ok);
}

TEST_F(NewerEnvelopeTest, AGenuineMessageAtItsSeqIsStillTaken) {
  ASSERT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(1, "one"), local_.account_id).decision,
            IngestDecision::AcceptBootstrap);
  (void)pipeline_->ProcessEnvelope(Newer(2), local_.account_id);
  const RelayReceiveOutcome genuine = pipeline_->ProcessEnvelope(TextEnvelope(2, "two"), local_.account_id);
  EXPECT_EQ(genuine.decision, IngestDecision::AcceptLateFill) << "a closed seq is a late fill, never a compromise";
  EXPECT_TRUE(genuine.persisted);
}

TEST_F(NewerEnvelopeTest, AtAnotherEpochOnlyTheRowIsShown) {
  ASSERT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(1, "one"), local_.account_id).decision,
            IngestDecision::AcceptBootstrap);
  RelayEnvelope envelope = TextEnvelope(1, "x");
  envelope.message_id = "newer-epoch";
  Object json = NewerEnvelopeJson(envelope);
  Object body = *json.getObject("body");
  Object e2e = *body.getObject("e2e");
  Object copy = *asObject(e2e.getArray("copies")->elements.front());
  copy.set("session_epoch", static_cast<int64_t>(5));
  e2e.set("copies", ArrayValue({ObjectValue(copy)}));
  body.set("e2e", e2e);
  json.set("body", body);
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(ParseRelayEnvelope(json).value(), local_.account_id);
  EXPECT_TRUE(outcome.persisted);
  EXPECT_EQ(store_.GetChatTargetSessionEpoch(thread_.id).value(), 1u) << "an unverified envelope moves no epoch";
  EXPECT_EQ(Sync().contiguous_peer_seq, 1u);
}

// ---- Envelope v4 (release-compatibility SIGNED_FORMATS § 2.1): algorithm ids, the extension area --------------------

pp::ExtArea Area(const uint16_t type) {
  return pp::ExtArea::FromEntries({{.type = type, .value = {1, 2}}}).value();
}

// Step 3: an algorithm id this build does not know (here the copy's AEAD) is a newer app's, before anything verifies:
// its seq closed, one "update to view" row, nothing of it verified or applied.
TEST_F(NewerEnvelopeTest, AnUnknownAlgorithmIdReadsAsANewerAppsEnvelope) {
  ASSERT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(1, "one"), local_.account_id).decision,
            IngestDecision::AcceptBootstrap);
  RelayEnvelope envelope = TextEnvelope(2, "two");
  envelope.signature = Base64Encode(ByteVector{0x02, 0x00, 0x01}); // signature algorithm 2: unknown, unverifiable
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(envelope, local_.account_id);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  EXPECT_FALSE(outcome.sender_verified);
  EXPECT_TRUE(Closed(Sync(), 2));
  EXPECT_EQ(Sync().contiguous_peer_seq, 2u);

  RelayEnvelope malformed = TextEnvelope(3, "three");
  malformed.signature = Base64Encode(ByteVector{0x01, 0x00}); // ml-dsa-65 of the wrong size
  EXPECT_EQ(pipeline_->ProcessEnvelope(malformed, local_.account_id).decision, IngestDecision::HardReject);
}

// Step 5: a critical extension this build does not understand is a newer app's once the signature verified — and a
// forgery when it was added after signing (an unsigned envelope never raises "update needed").
TEST_F(NewerEnvelopeTest, ACriticalExtensionIsReadOnlyAfterTheSignature) {
  ASSERT_EQ(pipeline_->ProcessEnvelope(TextEnvelope(1, "one"), local_.account_id).decision,
            IngestDecision::AcceptBootstrap);
  RelayEnvelope added_after = TextEnvelope(2, "two");
  added_after.ext = Area(0x8001);
  const RelayReceiveOutcome forged = pipeline_->ProcessEnvelope(added_after, local_.account_id);
  EXPECT_EQ(forged.decision, IngestDecision::HardReject);
  EXPECT_FALSE(forged.receive_failure);
  EXPECT_FALSE(Closed(Sync(), 2));

  RelayEnvelope critical = TextEnvelope(2, "two");
  critical.ext = Area(0x8001);
  test::SignEnvelopeAs(peer_, critical);
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(critical, local_.account_id);
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  EXPECT_TRUE(Closed(Sync(), 2));
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_TRUE(std::any_of(rows.begin(), rows.end(), [](const ThreadMessage& row) {
    return row.content_type == ChatContentType::Unsupported;
  })) << "update to view";
}

// An unknown non-critical entry is ignored (and signed): the message is read as any other.
TEST_F(NewerEnvelopeTest, ANonCriticalExtensionIsIgnored) {
  RelayEnvelope envelope = TextEnvelope(1, "one");
  envelope.ext = Area(0x0001);
  test::SignEnvelopeAs(peer_, envelope);
  auto parsed = ParseRelayEnvelope(RelayEnvelopeToJson(envelope));
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(parsed->ext, envelope.ext) << "kept as received";
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(*parsed, local_.account_id);
  EXPECT_EQ(outcome.decision, IngestDecision::AcceptBootstrap);
  EXPECT_TRUE(outcome.sender_verified);
}

// Step 1: an envelope before this version (or with none) is an older app's: refused as it is read.
TEST(TolerantEnvelopeParseTest, AnOlderOrMissingVersionIsRefused) {
  RelayEnvelope sent;
  sent.message_id = "older-1";
  sent.sender_contact_id = kPeer;
  sent.sender_device = "QmPeerDevice";
  sent.route.kind = kRelayRouteDirect;
  sent.route.channel = ThreadChannel::E2ePublic;
  sent.body.e2e.copies = {RelayCopy{.session_epoch = 1, .sender_seq = 1, .payload_b64 = "AQ"}};
  Object json = RelayEnvelopeToJson(sent);
  ASSERT_TRUE(ParseRelayEnvelope(json));
  json.set("envelope_version", int64_t{kRelayEnvelopeVersion - 1});
  auto older = ParseRelayEnvelope(json);
  ASSERT_FALSE(older);
  EXPECT_NE(older.error().message.find("older app"), std::string::npos) << older.error().message;
  EXPECT_FALSE(AppError::IsNewerFormat(older.error()));
  json.erase("envelope_version");
  EXPECT_FALSE(ParseRelayEnvelope(json)) << "no version: an older app's";
  json = RelayEnvelopeToJson(sent);
  json.set("ext_b64", "AAAA"); // three zero bytes: bytes left over
  EXPECT_FALSE(ParseRelayEnvelope(json)) << "a malformed area";
}

} // namespace
} // namespace pbr
