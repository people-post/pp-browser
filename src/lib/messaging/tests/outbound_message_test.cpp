// What a send of ours writes and puts on the wire (OutboundMessage), checked against the receive
// pipeline of the peer it goes to.
#include "lib/messaging/OutboundMessage.h"

#include "lib/messaging/EnvelopeSigner.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/PeerSigningKeyStore.h"
#include "lib/messaging/ReactionTypes.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/SqlitePskSessionStore.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/messaging/tests/TestAccount.h"
#include "common/chat/MessagingLimits.h"
#include "common/chat/RelayStreamKey.h"
#include "common/directory/DirectoryJson.h"
#include "common/Utilities.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"

#include <filesystem>
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
    store = std::make_unique<SqliteThreadStore>(root.string());
    EXPECT_TRUE(store->SetDek(TestDek(seed)) && store->ListThreads());
    roster = std::make_unique<GroupRosterStore>(store->ProfileDbPath());
    psk = std::make_unique<SqlitePskSessionStore>(store->ProfileDbPath(), tag);
    EXPECT_TRUE(psk->SetDek(TestDek(seed)));
    pipeline = std::make_unique<RelayReceivePipeline>(*store, resolver, *psk, keys.Keys(), *roster);
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

  void Know(const Device& peer) {
    PeerSigningKeyRecord record;
    record.signing_public_key_b64 = peer.keys.SigningPublicB64();
    record.source = "test";
    signing.Put(ContactIdKindToString(mesh::ContactIdKind::Account), peer.keys.account_id, record);
  }

  /** The direct send as MeshDeliveryOrchestrator runs it, minus the transports. */
  Roe<RelayEnvelope> Send(const Thread& thread, const Device& peer, const std::string& text,
                          const SendRelayOptions& options = {}) {
    auto appended = AppendOutboundMessage(*store, NewOutboundMessage(thread.id, text, options, util::GenerateUuid(),
                                                                     util::NowUnixMs()));
    if (!appended) {
      return appended.error();
    }
    auto sealed = SealDirectPayload(*appended, thread, keys.account_id, options, *psk,
                                    [&peer](const ChatTargetKey&) -> Roe<ByteVector> { return peer.keys.kem_public; });
    if (!sealed) {
      return sealed.error();
    }
    RelayEnvelope envelope = BuildDirectEnvelope(*appended, EnvelopeSender{"relay:" + tag, keys.account_id},
                                                 thread.channel, "relay:" + peer.tag, std::move(*sealed));
    auto bytes = EnvelopeSigner::BuildSignBytes(envelope);
    if (!bytes) {
      return bytes.error();
    }
    auto signature = keys.Sign(*bytes);
    if (!signature) {
      return signature.error();
    }
    envelope.signature = *signature;
    return envelope;
  }

  std::string tag;
  std::filesystem::path root;
  test::TestAccount keys;
  PeerSigningKeyStore signing;
  PeerSigningKeyResolver resolver{signing};
  std::unique_ptr<SqliteThreadStore> store;
  std::unique_ptr<GroupRosterStore> roster;
  std::unique_ptr<SqlitePskSessionStore> psk;
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
  EXPECT_TRUE(first->body.e2e.key_init_b64 && !first->body.e2e.key_init_b64->empty()) << "the opening send carries its key_init";
  EXPECT_EQ(first->route.kind, "direct");
  EXPECT_EQ(first->recipient_contact_id, "relay:bob");
  EXPECT_EQ(first->stream_key,
            BuildCanonicalRelayStreamKey("relay:alice", "relay:bob", ThreadChannel::E2ePublic, first->session_epoch));
  EXPECT_EQ(first->order_key, first->sender_seq);

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
    ASSERT_TRUE(AppendOutboundMessage(*alice_->store, NewOutboundMessage(thread.id, "👍", options,
                                                                         util::GenerateUuid(), 1)));
  }
  auto over = AppendOutboundMessage(*alice_->store, NewOutboundMessage(thread.id, "👍", options, "over", 1));
  ASSERT_FALSE(over);
  EXPECT_EQ(over.error().message, "Too many reactions on this message");

  SendRelayOptions retract = options;
  retract.payload_json = BuildReactionPayloadJson(kAnnotationTypeRetract, "msg-1", "");
  EXPECT_TRUE(AppendOutboundMessage(*alice_->store, NewOutboundMessage(thread.id, "", retract, "del", 1)))
      << "the cap never stops a delete";
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
}

TEST(OutboundSendRulesTest, AGroupMembersCopy) {
  ThreadMessage message;
  message.id = "m";
  message.sender_seq = 7;
  message.session_epoch = 2;
  message.timestamp = 5;
  RelayEnvelope e = BuildGroupMemberEnvelope(message, EnvelopeSender{"relay:a", "account:a"}, "group-1",
                                             "account:b", "relay:b", "payload", std::string("init"));
  EXPECT_EQ(e.route.kind, "group");
  EXPECT_EQ(e.route.group_id, "group-1");
  EXPECT_EQ(e.body.e2e.member_payloads->at("account:b"), "payload");
  EXPECT_EQ(e.body.e2e.key_init_b64, std::optional<std::string>("init"));
  EXPECT_EQ(e.stream_key, BuildGroupRelayStreamKey("group-1", 2, "account:b"));
  EXPECT_NE(BuildGroupMemberEnvelope(message, EnvelopeSender{"relay:a", "account:a"}, "group-1", "account:c",
                                     "relay:c", "payload", std::nullopt)
                .stream_key,
            e.stream_key)
      << "the relay keeps one row per sender, stream and seq: each member's copy has its own stream";
  EXPECT_EQ(e.recipient_contact_id, "relay:b") << "the relay delivers to relay ids";
  EXPECT_EQ(e.order_key, 7u);
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
  EXPECT_TRUE(OutboxRetryAgain(kMaxOutboxRetryAttempts - 1));
  EXPECT_FALSE(OutboxRetryAgain(kMaxOutboxRetryAttempts)) << "then it fails";
}

} // namespace
} // namespace pbr
