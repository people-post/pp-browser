// What a send of ours writes and puts on the wire (OutboundMessage), checked against the receive
// pipeline of the peer it goes to.
#include "lib/delivery/OutboundMessage.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "foundation/error/AppError.h"

#include "lib/delivery/EnvelopeSigner.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/people/PeerSigningKeyStore.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/EditMessage.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/tests/TestAccount.h"
#include "common/chat/MessagingLimits.h"
#include "common/chat/RelayStreamKey.h"
#include "common/directory/DirectoryJson.h"
#include "common/Utilities.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"

#include <filesystem>
#include <map>
#include <gtest/gtest.h>
#include <memory>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

ByteVector TestDek(uint8_t seed) {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(seed + i);
  }
  return dek;
}

/** One device's stores and keys. */
struct Device {
  Device(const std::string& tag, uint8_t seed) : tag(tag) {
    root = std::filesystem::temp_directory_path() / ("pp_outbound_" + tag + "_" + util::GenerateUuid());
    std::filesystem::create_directories(root);
    keys = test::TestAccount::Make();
    store = std::make_unique<InMemoryDeliveryStore>();
    EXPECT_TRUE(store->SetDek(TestDek(seed)) && store->ListThreads());
    roster = std::make_unique<InMemoryGroupRosterStore>(*store);
    psk = std::make_unique<InMemoryPeerSessions>(*store);
    EXPECT_TRUE(psk->SetDek(TestDek(seed)));
    pipeline = std::make_unique<RelayReceivePipeline>(
        *store,
        [this](const std::string& account) -> std::optional<AccountCard> {
          auto found = cards.find(account);
          return found == cards.end() ? std::nullopt : std::optional<AccountCard>(found->second);
        },
        *psk, test::ReceiveKeysOf(keys), *roster);
  }
  ~Device() {
    pipeline.reset();
    psk.reset();
    roster.reset();
    store.reset();
    std::filesystem::remove_all(root);
  }

  Thread DirectThreadTo(const Device& peer, ThreadChannel channel) {
    DirectChatTarget target;
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = peer.keys.account_id;
    target.channel = channel;
    auto thread = store->FindOrCreateDirectThread(target, "", "peer");
    EXPECT_TRUE(thread);
    return *thread;
  }

  void Know(const Device& peer) { cards[peer.keys.account_id] = peer.keys.DeviceCard(); }

  /** The direct send as MeshDeliveryOrchestrator runs it, minus the transports. */
  Roe<RelayEnvelope> Send(const Thread& thread, const Device& peer, const std::string& text,
                          const SendRelayOptions& options = {}) {
    auto appended = AppendOutboundMessage(
        *store, NewOutboundMessage(thread.id, text, options, util::GenerateUuid(), util::NowUnixMs()));
    if (!appended) {
      return appended.error();
    }
    auto sealed = SealDirectPayload(*appended, thread, keys.account_id, options, *psk,
                                    [&peer](const PeerSessionKey&) -> Roe<ByteVector> { return peer.keys.kem_public; });
    if (!sealed) {
      return sealed.error();
    }
    RelayEnvelope envelope = BuildDirectEnvelope(
        *appended, EnvelopeSender{"relay:" + tag, keys.account_id, keys.device.peer_id}, thread.channel,
        "relay:" + peer.tag, {AccountCopy(*appended, std::move(*sealed))}, NextRelayOrderKey());
    test::SignEnvelopeAs(keys, envelope);
    return envelope;
  }

  std::string tag;
  std::filesystem::path root;
  test::TestAccount keys;
  /** The peers' cards this device keeps. */
  std::map<std::string, AccountCard> cards;
  std::unique_ptr<InMemoryDeliveryStore> store;
  std::unique_ptr<InMemoryGroupRosterStore> roster;
  std::unique_ptr<InMemoryPeerSessions> psk;
  std::unique_ptr<RelayReceivePipeline> pipeline;
};

class OutboundMessageTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    alice_ = std::make_unique<Device>("alice", 0x10);
    bob_ = std::make_unique<Device>("bob", 0x40);
    alice_->Know(*bob_);
    bob_->Know(*alice_);
  }

  std::unique_ptr<Device> alice_;
  std::unique_ptr<Device> bob_;
};

TEST_F(OutboundMessageTest, AFirstPublicSendOpensTheSessionAndThePeerReadsIt) {
  const Thread thread = alice_->DirectThreadTo(*bob_, ThreadChannel::E2ePublic);
  auto first = alice_->Send(thread, *bob_, "hello");
  ASSERT_TRUE(first) << first.error().message;
  EXPECT_TRUE(first->body.e2e.key_init_b64 && !first->body.e2e.key_init_b64->empty())
      << "the opening send carries its key_init";
  EXPECT_EQ(first->route.kind, "direct");
  EXPECT_EQ(first->recipient_contact_id, "relay:bob");
  EXPECT_EQ(first->stream_key, BuildDeviceRelayStreamKey("relay:alice", "relay:bob", ThreadChannel::E2ePublic,
                                                         alice_->keys.device.peer_id))
      << "the sending device's stream: an account's devices share its relay id (M022)";
  ASSERT_EQ(first->body.e2e.copies.size(), 1u) << "one account copy: Bob's devices are not known yet";
  EXPECT_TRUE(first->body.e2e.copies.front().recipient_device.empty());

  auto second = alice_->Send(thread, *bob_, "again");
  ASSERT_TRUE(second);
  EXPECT_EQ(second->sender_seq, first->sender_seq + 1);
  EXPECT_EQ(second->body.e2e.key_init_b64, first->body.e2e.key_init_b64)
      << "announced until the peer is heard under it";

  const RelayReceiveOutcome got = bob_->pipeline->ProcessEnvelope(*first, bob_->keys.account_id);
  ASSERT_TRUE(got.persisted_message) << got.receive_failure_detail;
  EXPECT_EQ(got.persisted_message->text, "hello");
}

TEST_F(OutboundMessageTest, KeysTheSendCannotHave) {
  const Thread e2e = alice_->DirectThreadTo(*bob_, ThreadChannel::E2e);
  auto no_psk = alice_->Send(e2e, *bob_, "x");
  ASSERT_FALSE(no_psk);
  EXPECT_EQ(no_psk.error().message, "PSK not configured for this chat") << "a private chat never auto-keys";

  const Thread pub = alice_->DirectThreadTo(*bob_, ThreadChannel::E2ePublic);
  SendRelayOptions rotate;
  rotate.key_init_b64 = "AAAA";
  auto rotating = alice_->Send(pub, *bob_, "x", rotate);
  ASSERT_FALSE(rotating);
  EXPECT_EQ(rotating.error().message, "psk_rotate requires the current encryption key");
}

TEST_F(OutboundMessageTest, TheRowAndTheReactionCap) {
  const Thread thread = alice_->DirectThreadTo(*bob_, ThreadChannel::E2ePublic);
  SendRelayOptions options;
  options.content_type = ChatContentType::Annotation;
  options.payload_json = BuildReactionPayloadJson(kAnnotationTypeReaction, "msg-1", "👍");
  options.generation = "g";
  const ThreadMessage row = NewOutboundMessage(thread.id, "👍", options, "id-1", 42);
  EXPECT_EQ(row.target_message_id, std::optional<std::string>("msg-1"));
  EXPECT_EQ(row.delivery, MessageDelivery::Pending);
  EXPECT_EQ(row.sender_contact_id, kLocalSelfContactId);
  EXPECT_EQ(row.generation, std::optional<std::string>("g"));
  EXPECT_EQ(row.timestamp, 42);

  for (size_t i = 0; i < kMaxAnnotationsPerTarget; ++i) {
    ASSERT_TRUE(
        AppendOutboundMessage(*alice_->store, NewOutboundMessage(thread.id, "👍", options, util::GenerateUuid(), 1)));
  }
  auto over = AppendOutboundMessage(*alice_->store, NewOutboundMessage(thread.id, "👍", options, "over", 1));
  ASSERT_FALSE(over);
  EXPECT_EQ(over.error().message, "Too many reactions on this message");

  SendRelayOptions retract = options;
  retract.payload_json = BuildReactionPayloadJson(kAnnotationTypeRetract, "msg-1", "");
  EXPECT_TRUE(AppendOutboundMessage(*alice_->store, NewOutboundMessage(thread.id, "", retract, "del", 1)))
      << "the cap never stops a delete";
  EXPECT_TRUE(AppendOutboundMessage(
      *alice_->store, NewOutboundMessage(thread.id, "x", EditSendOptions("msg-1", "x", "ed", 7), "id", 1)))
      << "nor an edit";
}

// Our edit (DEV/PLAN-message-edit.md): it goes out as an `edit` annotation with the id and send time fixed at the tap,
// numbered and queued as any message (its own row here is what history serves later), our message reads the new text
// here at once, and the peer applies it.
TEST_F(OutboundMessageTest, AnEditGoesOutAsAnEditAnnotationStampedAtTheTap) {
  const Thread thread = alice_->DirectThreadTo(*bob_, ThreadChannel::E2ePublic);
  auto original = alice_->Send(thread, *bob_, "see you at 8");
  ASSERT_TRUE(original) << original.error().message;
  ASSERT_TRUE(bob_->pipeline->ProcessEnvelope(*original, bob_->keys.account_id).persisted);
  ThreadMessage target = alice_->store->GetMessage(thread.id, original->message_id).value().value();
  target.delivery = MessageDelivery::Relayed; // it went out
  ASSERT_TRUE(alice_->store->UpdateMessage(target));

  const int64_t tapped_at = target.timestamp + 14 * 60'000;
  ASSERT_TRUE(CanEditMessage(target, ThreadKind::Direct, tapped_at));
  const int64_t sent_at = EditSendTime(tapped_at, target);
  auto edit =
      alice_->Send(thread, *bob_, "see you at 9", EditSendOptions(target.id, "see you at 9", "edit-1", sent_at));
  ASSERT_TRUE(edit) << edit.error().message;
  EXPECT_EQ(edit->message_id, "edit-1");
  EXPECT_EQ(edit->timestamp, sent_at) << "stamped at the tap, whenever it is delivered";
  EXPECT_EQ(edit->sender_seq, original->sender_seq + 1) << "numbered as any message";

  const ThreadMessage row = alice_->store->GetMessage(thread.id, "edit-1").value().value();
  EXPECT_TRUE(IsEditAnnotation(row)) << "our own edit row stays here (history serves it)";
  EXPECT_EQ(row.target_message_id, std::optional<std::string>(target.id));
  EXPECT_EQ(EditTextOf(row), std::optional<std::string>("see you at 9"));
  EXPECT_EQ(row.delivery, MessageDelivery::Pending);
  const auto queued = alice_->store->ListPendingOutbox().value();
  EXPECT_TRUE(std::any_of(queued.begin(), queued.end(), [](const auto& entry) { return entry.first == "edit-1"; }))
      << "queued until it goes out, as any message";

  auto edited = ApplyOwnEdit(*alice_->store, target, row, {});
  ASSERT_TRUE(edited) << edited.error().message;
  const ThreadMessage ours = alice_->store->GetMessage(thread.id, target.id).value().value();
  EXPECT_EQ(ours.text, "see you at 9");
  EXPECT_EQ(EditMarkOf(ours)->edited_by, "edit-1");

  const RelayReceiveOutcome got = bob_->pipeline->ProcessEnvelope(*edit, bob_->keys.account_id);
  EXPECT_TRUE(got.persisted) << got.receive_failure_detail;
  EXPECT_TRUE(got.suppress_inbox_chrome);
  auto bobs = bob_->store->GetMessages(bob_->DirectThreadTo(*alice_, ThreadChannel::E2ePublic).id).value();
  const auto said = std::find_if(bobs.begin(), bobs.end(), [&](const ThreadMessage& m) { return m.id == target.id; });
  ASSERT_NE(said, bobs.end());
  EXPECT_EQ(said->text, "see you at 9");
  EXPECT_TRUE(EditMarkOf(*said).has_value());
}

/** The longest edit text of 'a's that EditFitsOnTheWire takes for `target_id`. */
size_t LongestEditThatFits(const std::string& target_id) {
  size_t fits = kMaxEditTextBytes;
  while (fits > 0 && !EditFitsOnTheWire(target_id, std::string(fits, 'a'))) {
    --fits;
  }
  return fits;
}

// An edit carries its text twice (top-level `text` and `value`), so a text the composer takes as a message can be too
// long as an edit. Such an edit is refused before anything is stored or sent: our message keeps its words, no edit row.
// The longest one that fits goes out, and the peer applies it.
TEST_F(OutboundMessageTest, AnEditTooLongForOnePayloadChangesNothingHereAndOneThatFitsGoesOut) {
  const Thread thread = alice_->DirectThreadTo(*bob_, ThreadChannel::E2ePublic);
  auto original = alice_->Send(thread, *bob_, "see you at 8");
  ASSERT_TRUE(original) << original.error().message;
  ASSERT_TRUE(bob_->pipeline->ProcessEnvelope(*original, bob_->keys.account_id).persisted);
  ThreadMessage target = alice_->store->GetMessage(thread.id, original->message_id).value().value();
  target.delivery = MessageDelivery::Relayed;
  ASSERT_TRUE(alice_->store->UpdateMessage(target));
  const int64_t tapped_at = target.timestamp + 60'000;

  const size_t fits = LongestEditThatFits(target.id);
  ASSERT_LT(fits, kMaxComposeTextBytes) << "the composer takes texts that do not fit as an edit";
  ThreadMessage over_wire;
  over_wire.content_type = ChatContentType::Annotation;
  over_wire.text = std::string(fits + 1, 'a');
  over_wire.target_message_id = target.id;
  over_wire.payload_json = BuildEditPayloadJson(target.id, over_wire.text);
  auto encoded = ChatPayloadCodec::EncodeToRow(over_wire);
  EXPECT_TRUE(!encoded || encoded->size() + kEditPayloadHeadroomBytes > kMaxE2ePlaintextBytes)
      << "one byte more is over the plaintext limit as the send encodes it";

  std::optional<RelayEnvelope> sent;
  const auto send = [&](const std::string& edit_id, const std::string& text, const int64_t sent_at_ms) {
    auto envelope = alice_->Send(thread, *bob_, text, EditSendOptions(target.id, text, edit_id, sent_at_ms));
    if (!envelope) {
      return Roe<ThreadMessage>(envelope.error());
    }
    sent = *envelope;
    return Roe<ThreadMessage>(alice_->store->GetMessage(thread.id, edit_id).value().value());
  };

  auto too_long = EditOwnMessage(*alice_->store, target, ThreadKind::Direct, std::string(fits + 1, 'a'), tapped_at,
                                 "edit-long", send, {});
  ASSERT_FALSE(too_long);
  EXPECT_EQ(too_long.error().message, "Edited text is empty or too long");
  EXPECT_FALSE(sent) << "nothing sent";
  EXPECT_FALSE(alice_->store->GetMessage(thread.id, "edit-long").value()) << "no edit row";
  EXPECT_TRUE(alice_->store->GetAnnotationsForTarget(thread.id, target.id).value().empty());
  const ThreadMessage kept = alice_->store->GetMessage(thread.id, target.id).value().value();
  EXPECT_EQ(kept.text, "see you at 8");
  EXPECT_FALSE(EditMarkOf(kept));

  auto just_fits = EditOwnMessage(*alice_->store, target, ThreadKind::Direct, std::string(fits, 'a'), tapped_at,
                                  "edit-fits", send, {});
  ASSERT_TRUE(just_fits) << just_fits.error().message;
  EXPECT_FALSE(just_fits->stored_unsent);
  EXPECT_EQ(just_fits->row.text, std::string(fits, 'a'));
  ASSERT_TRUE(sent);
  const RelayReceiveOutcome got = bob_->pipeline->ProcessEnvelope(*sent, bob_->keys.account_id);
  EXPECT_TRUE(got.persisted) << got.receive_failure_detail;
  const ThreadMessage bobs =
      bob_->store->GetMessage(bob_->DirectThreadTo(*alice_, ThreadChannel::E2ePublic).id, target.id).value().value();
  EXPECT_EQ(bobs.text, std::string(fits, 'a'));
}

TEST(OutboundSendRulesTest, WhoMaySendAndHow) {
  DirectSendGate gate;
  EXPECT_EQ(CheckDirectSendAllowed(gate).error().message, "Not a direct thread");
  gate.direct_thread = true;
  EXPECT_EQ(CheckDirectSendAllowed(gate).error().message, "Direct thread missing peer identity");
  gate.peer_known = true;
  EXPECT_TRUE(CheckDirectSendAllowed(gate));
  gate.locked_out = true;
  EXPECT_FALSE(CheckDirectSendAllowed(gate)) << "continues on another device";
  gate = DirectSendGate{};
  gate.direct_thread = true;
  gate.peer_known = true;
  gate.channel = ThreadChannel::E2e;
  gate.psk_ready = false;
  EXPECT_FALSE(CheckDirectSendAllowed(gate)) << "verify the key first";
  gate.psk_ready = true;
  gate.compromised = true;
  EXPECT_FALSE(CheckDirectSendAllowed(gate)) << "paused";

  EXPECT_TRUE(DirectSendTriesAmpFirst(true, true, false));
  EXPECT_FALSE(DirectSendTriesAmpFirst(true, true, true)) << "call control prefers a known relay route";
  EXPECT_TRUE(DirectSendTriesAmpFirst(true, false, true)) << "an Amp-only peer";
  EXPECT_FALSE(DirectSendTriesAmpFirst(false, false, false));

  // #431 (a): call control also goes over a live link, at once; nothing else changes.
  EXPECT_TRUE(ControlAlsoGoesDirect(true, true, true)) << "control, a live link, a relay route";
  EXPECT_FALSE(ControlAlsoGoesDirect(false, true, true)) << "no live link: the relay alone, as before";
  EXPECT_FALSE(ControlAlsoGoesDirect(true, false, true)) << "Amp-only peer: Amp alone, as before";
  EXPECT_FALSE(ControlAlsoGoesDirect(true, true, false)) << "chat: Amp first, then the relay, as before";
}

TEST(OutboundSendRulesTest, AGroupMembersEnvelopeCarriesACopyPerDevice) {
  ThreadMessage message;
  message.id = "m";
  message.sender_seq = 7;
  message.session_epoch = 2;
  message.timestamp = 5;
  const EnvelopeSender sender{"relay:a", "account:a", "QmA"};
  const RelayCopy phone{.recipient_device = "QmBPhone",
                        .session_epoch = 5,
                        .sender_seq = 9,
                        .key_init_b64 = std::string("init"),
                        .payload_b64 = "cGF5bG9hZA=="};
  const RelayCopy laptop{
      .recipient_device = "QmBLaptop", .session_epoch = 1, .sender_seq = 3, .payload_b64 = "cGF5bG9hZDI="};
  RelayEnvelope e = BuildGroupMemberEnvelope(message, sender, "group-1", "account:b", "relay:b", {phone, laptop}, 11);
  EXPECT_EQ(e.route.kind, "group");
  EXPECT_EQ(e.body.e2e.copies, (std::vector<RelayCopy>{phone, laptop})) << "one record, each device's copy (M022)";
  EXPECT_EQ(e.order_key, 11u) << "the record's own relay index";
  EXPECT_EQ(e.stream_key, BuildGroupRelayStreamKey("group-1", "account:a", "account:b", "QmA"));
  EXPECT_NE(BuildGroupMemberEnvelope(message, sender, "group-1", "account:c", "relay:c", {phone}, 12).stream_key,
            e.stream_key)
      << "the relay keeps one row per sender, stream and index: each member's records have their own stream";
  EXPECT_EQ(e.recipient_contact_id, "relay:b") << "the relay delivers to relay ids";
  EXPECT_EQ(e.relay_wake_hint, "m") << "a tap on the push is matched to this message by its id";
  // Each device reads its own copy, numbered in its own pair.
  auto on_laptop = SelectRelayCopy(e, "QmBLaptop");
  ASSERT_TRUE(on_laptop);
  EXPECT_EQ(on_laptop->sender_seq, 3u);
  EXPECT_EQ(on_laptop->session_epoch, 1u);
  EXPECT_FALSE(on_laptop->body.e2e.key_init_b64);
  EXPECT_FALSE(SelectRelayCopy(e, "QmBTablet")) << "no copy for a device the sender did not know";
}

} // namespace
} // namespace pbr

namespace pbr {
namespace {

TEST(OutboxRetryTest, WhatIsStillWorthSending) {
  EXPECT_TRUE(OutboxSendStillWanted(/*control_lane=*/true, kControlRetryWindowMs, false));
  EXPECT_FALSE(OutboxSendStillWanted(true, kControlRetryWindowMs + 1, false)) << "signaling past its use";
  EXPECT_TRUE(OutboxSendStillWanted(false, kControlRetryWindowMs * 100, false)) << "a chat message waits";
  EXPECT_FALSE(OutboxSendStillWanted(false, 0, /*thread_paused=*/true));
  const Error unreachable = AppError::Network(Err::Network::Unreachable, "relay down");
  EXPECT_TRUE(OutboxRetryAgain(kMaxOutboxRetryAttempts - 1, unreachable));
  EXPECT_FALSE(OutboxRetryAgain(kMaxOutboxRetryAttempts, unreachable)) << "then it fails";
  EXPECT_TRUE(OutboxRetryAgain(1, AppError::Network(Err::Network::HttpError, "502"))) << "a server hiccup";
  // #391: the relay refused the request itself (a 400): the same bytes would be refused again.
  EXPECT_FALSE(OutboxRetryAgain(1, AppError::Network(Err::Network::HttpRejected, "400")));
}

TEST(OutboxRetryTest, TheControlLaneBacksOffWithinItsWindow) {
  using std::chrono::milliseconds;
  const Error unreachable = AppError::Network(Err::Network::Unreachable, "relay down");
  EXPECT_EQ(ControlRetryAfter(1, 0, unreachable), milliseconds(1000)) << "live signaling goes again at once";
  EXPECT_EQ(ControlRetryAfter(2, 0, unreachable), milliseconds(2000));
  EXPECT_EQ(ControlRetryAfter(4, 0, unreachable), milliseconds(8000));
  EXPECT_EQ(ControlRetryAfter(5, 0, unreachable), kControlRetryMaxDelay);
  EXPECT_EQ(ControlRetryAfter(40, 0, unreachable), kControlRetryMaxDelay) << "not bound by the wake's attempt cap";
  EXPECT_EQ(ControlRetryAfter(1, kControlRetryWindowMs - 1000, unreachable), milliseconds(1000));
  EXPECT_FALSE(ControlRetryAfter(1, kControlRetryWindowMs - 999, unreachable)) << "it would land too late";
  EXPECT_FALSE(ControlRetryAfter(1, 0, AppError::Network(Err::Network::HttpRejected, "400")));
}

} // namespace
} // namespace pbr
