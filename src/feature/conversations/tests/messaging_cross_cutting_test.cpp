#include "foundation/crypto/CryptoUtil.h"
#include "domain/messaging/ChatPayloadCodec.h"
#include "domain/messaging/E2eRelayPayloadCodec.h"
#include "domain/messaging/EnvelopeSigner.h"
#include "common/chat/MessagingLimits.h"
#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/MlDsa.h"
#include "domain/people/IdentityStore.h"
#include "domain/messaging/RelayWirePayload.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/MlDsa.h"
#include "domain/people/IdentityStore.h"
#include "domain/messaging/GroupRosterStore.h"
#include "feature/conversations/RelayReceivePipeline.h"
#include "domain/messaging/SqlitePskSessionStore.h"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {

using namespace pbr;

ByteVector TestMasterPsk() {
  const auto bytes = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  EXPECT_TRUE(bytes);
  return *bytes;
}

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

class PipelineHarness {
public:
  explicit   PipelineHarness(const std::string& suffix, const ThreadChannel channel)
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_cross_cutting_" + suffix)),
        store(data_dir.string()),
        identity(data_dir.string(), "test"),
        roster_store(store.ProfileDbPath()),
        psk_store(store.ProfileDbPath(), "test"),
        key_resolver(key_store),
        pipeline(store, key_resolver, psk_store, identity, roster_store) {
    std::filesystem::remove_all(data_dir);
    if (!identity.SetDek(TestDek()) || !psk_store.SetDek(TestDek()) || !store.SetDek(TestDek())) {
      throw std::runtime_error("Failed to set test DEK");
    }

    auto generated = MlDsa::GenerateKeyPair();
    if (!generated) {
      throw std::runtime_error("Failed to generate peer keys");
    }
    peer_keys = *generated;

    PeerSigningKeyRecord record;
    record.signing_public_key_b64 = Base64Encode(peer_keys.public_key);
    record.source = "test";
    key_store.Put("account", "account:peer", record);

    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = "account:peer";
    target.channel = channel;

    auto created = store.FindOrCreateDirectThread(target, "contact-peer", "Peer");
    if (!created) {
      throw std::runtime_error("Failed to create thread");
    }
    thread = *created;

    if (!identity.LoadOrCreate()) {
      throw std::runtime_error("Failed to load local identity");
    }
    local_account_id = identity.Get()->account_id;

    if (channel == ThreadChannel::E2e) {
      PskSessionRecord psk;
      psk.key = E2eRelayPayloadCodec::ChatTargetFromThread(thread);
      psk.session_epoch = 1;
      psk.master_psk_b64 = Base64Encode(TestMasterPsk());
      psk.psk_verified_at = 1;
      if (!psk_store.Save(psk)) {
        throw std::runtime_error("Failed to save test PSK");
      }
    } else if (channel == ThreadChannel::E2ePublic) {
      auto kem_private = identity.GetOrCreateHybridKemPrivateKey();
      if (!kem_private) {
        throw std::runtime_error("Failed to load local KEM key");
      }
      local_kem_private = std::move(*kem_private);
    }
  }

  RelayEnvelope MakeSignedEnvelope(const std::string& message_id, uint64_t seq, const std::string& text,
                                   const ThreadChannel channel) const {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = message_id;
    envelope.sender_contact_id = "account:peer";
    envelope.sender_relay_id = "relay:peer";
    envelope.route.kind = "direct";
    envelope.route.channel = channel;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = static_cast<int64_t>(seq);

    if (channel == ThreadChannel::E2e) {
      E2eEncryptParams params;
      params.text = text;
      params.channel = CryptoChannel::E2e;
      params.peer_contact_id = local_account_id;
      params.sender_contact_id = "account:peer";
      params.message_id = envelope.message_id;
      params.sender_seq = seq;
      params.session_epoch = 1;
      params.timestamp = envelope.timestamp;
      auto payload = E2eRelayPayloadCodec::EncryptText(params, TestMasterPsk());
      if (!payload) {
        throw std::runtime_error("Failed to encrypt payload");
      }
      envelope.body.e2e.payload_b64 = *payload;
    } else if (channel == ThreadChannel::E2ePublic) {
      auto local_public_b64 = identity.GetHybridKemPublicKeyB64();
      if (!local_public_b64) {
        throw std::runtime_error("Missing local KEM public key");
      }
      auto local_public = Base64Decode(*local_public_b64);
      if (!local_public) {
        throw std::runtime_error("Failed to decode local KEM public key");
      }
      auto established = AutoKeyEstablishment::EncapsulateForRecipient(*local_public);
      if (!established) {
        throw std::runtime_error("Failed to establish auto-key");
      }
      E2eEncryptParams params;
      params.text = text;
      params.channel = CryptoChannel::E2ePublic;
      params.peer_contact_id = local_account_id;
      params.sender_contact_id = "account:peer";
      params.message_id = envelope.message_id;
      params.sender_seq = seq;
      params.session_epoch = 1;
      params.timestamp = envelope.timestamp;
      auto payload = E2eRelayPayloadCodec::EncryptTextWithAutoKey(params, established->master_psk,
                                                                  established->key_init_b64);
      if (!payload) {
        throw std::runtime_error("Failed to encrypt public payload");
      }
      envelope.body.e2e.payload_b64 = payload->payload_b64;
      envelope.body.e2e.key_init_b64 = payload->key_init_b64;
    } else {
      auto payload = RelayWirePayload::EncodePlaintextText(text);
      if (!payload) {
        throw std::runtime_error("Failed to encode plaintext payload");
      }
      envelope.body.e2e.payload_b64 = *payload;
    }

    auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
    if (!sign_bytes) {
      throw std::runtime_error("Failed to build sign bytes");
    }
    auto signature = MlDsa::Sign(peer_keys.secret_key, *sign_bytes);
    if (!signature) {
      throw std::runtime_error("Failed to sign envelope");
    }
    envelope.signature = Base64Encode(*signature);
    return envelope;
  }

  std::filesystem::path data_dir;
  SqliteThreadStore store;
  IdentityStore identity;
  GroupRosterStore roster_store;
  SqlitePskSessionStore psk_store;
  PeerSigningKeyStore key_store;
  PeerSigningKeyResolver key_resolver;
  RelayReceivePipeline pipeline;
  Thread thread;
  MlDsaKeyPair peer_keys;
  std::string local_account_id;
  std::optional<ByteVector> local_kem_private;
};

} // namespace

TEST(MessagingCrossCuttingTest, DuplicateRelayMessageIdIsBenignDuplicate) {
  PipelineHarness harness("dedup", ThreadChannel::E2e);
  const RelayEnvelope envelope = harness.MakeSignedEnvelope("dup-msg-1", 1, "hello", ThreadChannel::E2e);

  const RelayReceiveOutcome first = harness.pipeline.ProcessEnvelope(envelope, harness.local_account_id);
  EXPECT_EQ(first.decision, IngestDecision::AcceptBootstrap);
  EXPECT_TRUE(first.persisted);

  const RelayReceiveOutcome second = harness.pipeline.ProcessEnvelope(envelope, harness.local_account_id);
  EXPECT_EQ(second.decision, IngestDecision::BenignDuplicate);
  EXPECT_FALSE(second.persisted);

  auto has_id = harness.store.HasMessageId(harness.thread.id, "dup-msg-1");
  ASSERT_TRUE(static_cast<bool>(has_id));
  EXPECT_TRUE(*has_id);
}

TEST(MessagingCrossCuttingTest, OversizeEnvelopeHardRejected) {
  PipelineHarness harness("oversize", ThreadChannel::E2ePublic);

  RelayEnvelope envelope = harness.MakeSignedEnvelope("oversize-msg", 1, "x", ThreadChannel::E2ePublic);
  const ByteVector huge(kMaxRelayEnvelopeBytes + 1, static_cast<uint8_t>('a'));
  envelope.body.e2e.payload_b64 = Base64Encode(huge);

  const RelayReceiveOutcome outcome = harness.pipeline.ProcessEnvelope(envelope, harness.local_account_id);
  EXPECT_EQ(outcome.decision, IngestDecision::HardReject);
  EXPECT_FALSE(outcome.persisted);
}

TEST(MessagingCrossCuttingTest, FindOnlyRejectsUnknownSenderThread) {
  PipelineHarness harness("find_only", ThreadChannel::E2e);

  RelayEnvelope envelope = harness.MakeSignedEnvelope("unknown-peer-msg", 1, "hello", ThreadChannel::E2e);
  envelope.sender_contact_id = "account:stranger";
  envelope.sender_relay_id = "relay:stranger";

  const RelayReceiveOutcome outcome = harness.pipeline.ProcessEnvelope(envelope, harness.local_account_id);
  EXPECT_EQ(outcome.decision, IngestDecision::HardReject);
  EXPECT_FALSE(outcome.persisted);
  // Stale/unknown peer traffic is silent — no receive_failure UI spam.
  EXPECT_FALSE(outcome.receive_failure_notice.has_value());
}

TEST(MessagingCrossCuttingTest, E2ePublicIngestUsesEncryptedAutoKeyPath) {
  PipelineHarness harness("e2e_public", ThreadChannel::E2ePublic);
  const RelayEnvelope envelope =
      harness.MakeSignedEnvelope("public-msg-1", 1, "public hello", ThreadChannel::E2ePublic);

  const RelayReceiveOutcome outcome = harness.pipeline.ProcessEnvelope(envelope, harness.local_account_id);
  EXPECT_EQ(outcome.decision, IngestDecision::AcceptBootstrap);
  EXPECT_TRUE(outcome.persisted);

  auto messages = harness.store.GetMessages(harness.thread.id);
  ASSERT_TRUE(static_cast<bool>(messages));
  ASSERT_EQ(messages->size(), 1u);
  EXPECT_EQ(messages->front().text, "public hello");
}

TEST(MessagingCrossCuttingTest, ChatTargetRoutingFindsDirectThread) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_cross_cutting_routing";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.SetDek(TestDek()));

  DirectChatTarget target;
  target.peer_identity_kind = "account";
  target.peer_identity_value = "account:routed";
  target.channel = ThreadChannel::E2e;

  auto created = store.FindOrCreateDirectThread(target, "contact-routed", "Routed");
  ASSERT_TRUE(static_cast<bool>(created));

  auto found = store.FindDirectThread(target);
  ASSERT_TRUE(static_cast<bool>(found));
  ASSERT_TRUE(found->has_value());
  EXPECT_EQ(found->value().id, created->id);
}

TEST(MessagingCrossCuttingTest, RichPayloadMetadataSurvivesSqliteRestart) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_cross_cutting_rich_meta";
  std::filesystem::remove_all(data_dir);

  Thread thread;
  thread.id = "thread-rich-meta";
  thread.kind = ThreadKind::Ai;
  thread.title = "Rich";

  {
    SqliteThreadStore store(data_dir.string());
    ASSERT_TRUE(store.SetDek(TestDek()));
    ASSERT_TRUE(store.UpsertThread(thread));

    ThreadMessage message;
    message.id = "annotation-1";
    message.thread_id = thread.id;
    message.sender_contact_id = kLocalSelfContactId;
    message.content_type = ChatContentType::Annotation;
    message.text = "note";
    message.target_message_id = "target-1";
    message.timestamp = 99;
    ASSERT_TRUE(static_cast<bool>(store.AppendMessage(message)));
    store.Flush();
  }

  SqliteThreadStore store(data_dir.string());
  ASSERT_TRUE(store.SetDek(TestDek()));
  auto page = store.GetMessagesPage(thread.id, std::nullopt, 10);
  ASSERT_TRUE(static_cast<bool>(page));
  ASSERT_EQ(page->size(), 1u);
  EXPECT_EQ(page->front().content_type, ChatContentType::Annotation);
  ASSERT_TRUE(page->front().target_message_id.has_value());
  EXPECT_EQ(*page->front().target_message_id, "target-1");
}
