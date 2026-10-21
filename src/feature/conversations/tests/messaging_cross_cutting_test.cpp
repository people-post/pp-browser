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
#include "domain/messaging/ReactionTypes.h"
#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/MlDsa.h"
#include "domain/people/IdentityStore.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/ControlLaneEnvelope.h"
#include "domain/messaging/ControlLaneSeenStore.h"
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

  /** A peer E2E envelope carrying any chat payload (annotations etc.), signed like the text one. */
  RelayEnvelope MakeSignedPayloadEnvelope(const std::string& message_id, uint64_t seq, const ThreadMessage& body) const {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = message_id;
    envelope.sender_contact_id = "account:peer";
    envelope.sender_relay_id = "relay:peer";
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2e;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = static_cast<int64_t>(seq);
    auto row = ChatPayloadCodec::EncodeToRow(body);
    if (!row) {
      throw std::runtime_error("Failed to encode payload");
    }
    E2eEncryptParams params;
    params.text = body.text;
    params.channel = CryptoChannel::E2e;
    params.peer_contact_id = local_account_id;
    params.sender_contact_id = "account:peer";
    params.message_id = message_id;
    params.sender_seq = seq;
    params.session_epoch = 1;
    params.timestamp = envelope.timestamp;
    auto payload = E2eRelayPayloadCodec::EncryptChatPayloadBytes(params, *row, TestMasterPsk());
    if (!payload) {
      throw std::runtime_error("Failed to encrypt payload");
    }
    envelope.body.e2e.payload_b64 = *payload;
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

  /**
   * The peer's control-lane envelope carrying `body`, sent at `sent_ms` — built by the product's
   * BuildControlLaneEnvelope, the peer opening the pair's session once (AutoKey to our KEM key).
   */
  RelayEnvelope MakeControlEnvelope(const std::string& message_id, const ThreadMessage& body, int64_t sent_ms) const {
    if (!lane_session) {
      auto local_public_b64 = identity.GetHybridKemPublicKeyB64();
      auto local_public = local_public_b64 ? Base64Decode(*local_public_b64) : Roe<ByteVector>(Error("no kem"));
      auto opened = local_public ? AutoKeyEstablishment::EncapsulateForRecipient(*local_public)
                                 : Roe<AutoKeyEncapsulation>(local_public.error());
      if (!opened) {
        throw std::runtime_error("Failed to establish auto-key");
      }
      lane_session = *opened;
    }
    ControlLaneSendParams params;
    params.sender_account_id = "account:peer";
    params.sender_relay_id = "relay:peer";
    params.peer_account_id = local_account_id;
    params.route_party = "relay:local";
    params.session_epoch = 1;
    params.master_psk = lane_session->master_psk;
    params.key_init_b64 = lane_session->key_init_b64;
    params.order_key = 1;
    ThreadMessage control = body;
    control.id = message_id;
    control.timestamp = sent_ms;
    auto envelope = BuildControlLaneEnvelope(params, control, [this](const std::vector<uint8_t>& bytes) -> Roe<std::string> {
      auto signature = MlDsa::Sign(peer_keys.secret_key, bytes);
      if (!signature) {
        return signature.error();
      }
      return Base64Encode(*signature);
    });
    if (!envelope) {
      throw std::runtime_error("Failed to build control envelope: " + envelope.error().message);
    }
    return *envelope;
  }

  RelayEnvelope MakeRetract(const std::string& message_id, uint64_t seq, const std::string& target_id) const {
    ThreadMessage retract;
    retract.content_type = ChatContentType::Annotation;
    retract.target_message_id = target_id;
    retract.payload_json = BuildReactionPayloadJson(kAnnotationTypeRetract, target_id, "");
    return MakeSignedPayloadEnvelope(message_id, seq, retract);
  }

  std::optional<ThreadMessage> Stored(const std::string& id) {
    auto page = store.GetMessagesPage(thread.id, std::nullopt, 100);
    if (!page) {
      return std::nullopt;
    }
    for (const ThreadMessage& m : *page) {
      if (m.id == id) {
        return m;
      }
    }
    return std::nullopt;
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
  /** The peer's e2e_public session as its control-lane sends use it. */
  mutable std::optional<AutoKeyEncapsulation> lane_session;
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

// "Delete for everyone" from the peer: their message becomes a tombstone here, quietly.
TEST(MessagingCrossCuttingTest, PeerRetractTombstonesTheirMessage) {
  PipelineHarness harness("retract", ThreadChannel::E2e);
  std::vector<std::string> retracted;
  harness.pipeline.SetOnMessageRetracted(
      [&](const std::string&, const ThreadMessage& original) { retracted.push_back(original.text); });
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeSignedEnvelope("peer-1", 1, "oops", ThreadChannel::E2e),
                                   harness.local_account_id)
                  .persisted);

  const RelayReceiveOutcome outcome =
      harness.pipeline.ProcessEnvelope(harness.MakeRetract("retract-1", 2, "peer-1"), harness.local_account_id);
  EXPECT_TRUE(outcome.persisted);
  EXPECT_TRUE(outcome.deleted_by_sender);
  EXPECT_TRUE(outcome.suppress_inbox_chrome);
  const auto target = harness.Stored("peer-1");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->content_type, ChatContentType::Deleted);
  EXPECT_TRUE(target->text.empty());
  ASSERT_EQ(retracted.size(), 1u);
  EXPECT_EQ(retracted[0], "oops");
}

// The retract can overtake its message: the message is then stored as a tombstone on arrival.
TEST(MessagingCrossCuttingTest, MessageArrivingAfterItsRetractIsStoredDeleted) {
  PipelineHarness harness("retract_first", ThreadChannel::E2e);
  const RelayReceiveOutcome first =
      harness.pipeline.ProcessEnvelope(harness.MakeRetract("retract-1", 2, "peer-1"), harness.local_account_id);
  ASSERT_TRUE(first.persisted);
  const RelayReceiveOutcome late = harness.pipeline.ProcessEnvelope(
      harness.MakeSignedEnvelope("peer-1", 1, "oops", ThreadChannel::E2e), harness.local_account_id, true);
  EXPECT_TRUE(late.persisted);
  EXPECT_TRUE(late.deleted_by_sender);
  const auto target = harness.Stored("peer-1");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->content_type, ChatContentType::Deleted);
  EXPECT_TRUE(target->text.empty());
}

// Only a message's own sender can withdraw it: a peer's retract aimed at our message does nothing.
TEST(MessagingCrossCuttingTest, RetractOfSomeoneElsesMessageIsIgnored) {
  PipelineHarness harness("retract_other", ThreadChannel::E2e);
  ThreadMessage mine;
  mine.id = "mine-1";
  mine.thread_id = harness.thread.id;
  mine.sender_contact_id = kLocalSelfContactId;
  mine.text = "keep me";
  mine.timestamp = 1;
  ASSERT_TRUE(harness.store.AppendMessage(mine));
  const RelayReceiveOutcome outcome =
      harness.pipeline.ProcessEnvelope(harness.MakeRetract("retract-1", 1, "mine-1"), harness.local_account_id);
  EXPECT_TRUE(outcome.persisted);
  const auto target = harness.Stored("mine-1");
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->content_type, ChatContentType::Text);
  EXPECT_EQ(target->text, "keep me");
}

// History placeholder for a message the sender deleted: a retract aimed at its own id fills the seq and is
// stored as "This message was deleted".
TEST(MessagingCrossCuttingTest, HistoryPlaceholderIsStoredDeleted) {
  PipelineHarness harness("retract_placeholder", ThreadChannel::E2e);
  const RelayReceiveOutcome outcome =
      harness.pipeline.ProcessEnvelope(harness.MakeRetract("gone-1", 1, "gone-1"), harness.local_account_id);
  EXPECT_TRUE(outcome.persisted);
  EXPECT_TRUE(outcome.deleted_by_sender);
  const auto row = harness.Stored("gone-1");
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(row->content_type, ChatContentType::Deleted);
}

// Only a message can be withdrawn: a retract aimed at the sender's own reaction does nothing.
TEST(MessagingCrossCuttingTest, RetractOfAReactionIsIgnored) {
  PipelineHarness harness("retract_reaction", ThreadChannel::E2e);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeSignedEnvelope("peer-1", 1, "hello", ThreadChannel::E2e),
                                   harness.local_account_id)
                  .persisted);
  ThreadMessage reaction;
  reaction.content_type = ChatContentType::Annotation;
  reaction.target_message_id = "peer-1";
  reaction.text = "x";
  reaction.payload_json = BuildReactionPayloadJson(kAnnotationTypeReaction, "peer-1", "x");
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeSignedPayloadEnvelope("react-1", 2, reaction), harness.local_account_id)
                  .persisted);
  ASSERT_TRUE(harness.pipeline.ProcessEnvelope(harness.MakeRetract("retract-1", 3, "react-1"), harness.local_account_id)
                  .persisted);
  const auto row = harness.Stored("react-1");
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(row->content_type, ChatContentType::Annotation);
}

// The reaction cap never stops a delete.
TEST(MessagingCrossCuttingTest, RetractPassesTheAnnotationCap) {
  PipelineHarness harness("retract_cap", ThreadChannel::E2e);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeSignedEnvelope("peer-1", 1, "hello", ThreadChannel::E2e),
                                   harness.local_account_id)
                  .persisted);
  for (size_t i = 0; i < kMaxAnnotationsPerTarget; ++i) {
    ThreadMessage filler;
    filler.id = "fill-" + std::to_string(i);
    filler.thread_id = harness.thread.id;
    filler.sender_contact_id = kLocalSelfContactId;
    filler.content_type = ChatContentType::Annotation;
    filler.target_message_id = "peer-1";
    filler.text = "x";
    filler.payload_json = BuildReactionPayloadJson(kAnnotationTypeReaction, "peer-1", "x");
    filler.timestamp = 2;
    ASSERT_TRUE(harness.store.AppendMessage(filler));
  }
  const RelayReceiveOutcome outcome =
      harness.pipeline.ProcessEnvelope(harness.MakeRetract("retract-1", 2, "peer-1"), harness.local_account_id);
  EXPECT_TRUE(outcome.persisted);
  const auto row = harness.Stored("peer-1");
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(row->content_type, ChatContentType::Deleted);
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

// System-control families (call control, …): messaging routes a feature's control messages to the
// family that claims them, without knowing what they mean.
namespace {

ThreadMessage TestControl(const std::string& control_type) {
  ThreadMessage message;
  message.content_type = ChatContentType::System;
  message.text = "control";
  message.payload_json = std::string(R"({"control_type":")") + control_type + R"(","detail":"{}"})";
  return message;
}

struct TestFamily {
  int applied = 0;
  bool fail = false;

  SystemControlFamily Make(const std::string& name, const std::string& control_type, bool lane, bool quiet) {
    SystemControlFamily family;
    family.name = name;
    family.owns = [control_type](const ThreadMessage& message) {
      return message.payload_json.find("\"" + control_type + "\"") != std::string::npos;
    };
    family.apply = [this](ThreadMessage&, const SystemControlContext& context) -> Roe<void> {
      EXPECT_EQ(context.sender_identity, "account:peer");
      ++applied;
      if (fail) {
        return Error("refused by the family");
      }
      return {};
    };
    if (lane) {
      family.on_lane = [](const ThreadMessage&) { return true; };
      family.lane_max_age_ms = 60'000;
    }
    family.quiet = [quiet](const ThreadMessage&) { return quiet; };
    return family;
  }
};

/** A pipeline harness with the lane's seen-store wired. */
struct LaneHarness {
  explicit LaneHarness(const std::string& suffix)
      : harness(suffix, ThreadChannel::E2ePublic), seen(harness.store.ProfileDbPath()) {
    harness.pipeline.SetSystemControls(&controls);
    harness.pipeline.SetControlLaneSeen(&seen);
  }
  RelayReceiveOutcome Ingest(const RelayEnvelope& envelope) {
    return harness.pipeline.ProcessEnvelope(envelope, harness.local_account_id);
  }
  size_t StoredCount() {
    auto page = harness.store.GetMessagesPage(harness.thread.id, std::nullopt, 100);
    return page ? page->size() : 0;
  }

  SystemControlRegistry controls;
  PipelineHarness harness;
  ControlLaneSeenStore seen;
};

} // namespace

TEST(MessagingCrossCuttingTest, StreamControlFamilyAppliesAndKeepsTheInboxQuiet) {
  PipelineHarness harness("control_quiet", ThreadChannel::E2e);
  SystemControlRegistry controls;
  TestFamily family;
  controls.Bind(family.Make("ping", "test_ping", false, true));
  harness.pipeline.SetSystemControls(&controls);

  const auto envelope = harness.MakeSignedPayloadEnvelope("ctl-1", 1, TestControl("test_ping"));
  const RelayReceiveOutcome first = harness.pipeline.ProcessEnvelope(envelope, harness.local_account_id);
  EXPECT_TRUE(first.persisted) << "a chat-stream control message is a message: stored, end to end";
  EXPECT_TRUE(first.suppress_inbox_chrome);
  EXPECT_EQ(family.applied, 1);

  const RelayReceiveOutcome again = harness.pipeline.ProcessEnvelope(envelope, harness.local_account_id);
  EXPECT_EQ(again.decision, IngestDecision::BenignDuplicate);
  EXPECT_EQ(family.applied, 1) << "dropped by the ordered stream, not applied again";
}

TEST(MessagingCrossCuttingTest, LaneMessageAppliesOnceAndIsNeverStored) {
  LaneHarness lane("lane_once");
  TestFamily family;
  lane.controls.Bind(family.Make("call", "test_signal", true, true));
  const auto envelope = lane.harness.MakeControlEnvelope("lane-1", TestControl("test_signal"), util::NowUnixMs());

  const RelayReceiveOutcome first = lane.Ingest(envelope);
  EXPECT_EQ(first.decision, IngestDecision::AcceptControl);
  EXPECT_FALSE(first.persisted);
  EXPECT_EQ(family.applied, 1);
  EXPECT_EQ(lane.StoredCount(), 0u) << "no transcript row, no twin thread";

  const RelayReceiveOutcome again = lane.Ingest(envelope);  // relay redelivery / second transport
  EXPECT_EQ(again.decision, IngestDecision::BenignDuplicate);
  EXPECT_EQ(family.applied, 1);
}

TEST(MessagingCrossCuttingTest, LaneMessageInTheChatStreamIsDropped) {
  PipelineHarness harness("lane_in_stream", ThreadChannel::E2e);
  SystemControlRegistry controls;
  TestFamily family;
  controls.Bind(family.Make("call", "test_signal", true, false));
  harness.pipeline.SetSystemControls(&controls);

  const RelayReceiveOutcome outcome = harness.pipeline.ProcessEnvelope(
      harness.MakeSignedPayloadEnvelope("stream-signal", 1, TestControl("test_signal")), harness.local_account_id);
  EXPECT_EQ(outcome.decision, IngestDecision::SilentDiscard);
  EXPECT_FALSE(outcome.persisted);
  EXPECT_EQ(family.applied, 0);
  EXPECT_FALSE(harness.Stored("stream-signal").has_value());
}

TEST(MessagingCrossCuttingTest, StaleLaneMessageIsDropped) {
  LaneHarness lane("lane_stale");
  TestFamily family;
  lane.controls.Bind(family.Make("call", "test_signal", true, false));
  const int64_t now = util::NowUnixMs();

  // Waited past the family's age in the relay (relay clock: no skew between devices).
  auto waited = lane.harness.MakeControlEnvelope("lane-waited", TestControl("test_signal"), now);
  waited.relay_created_at_ms = 1'000'000;
  waited.relay_server_time_ms = 1'000'000 + 61'000;
  EXPECT_EQ(lane.Ingest(waited).decision, IngestDecision::SilentDiscard);

  // Sent long ago by the sender's signed clock, whatever the relay says.
  auto old = lane.harness.MakeControlEnvelope("lane-old", TestControl("test_signal"), now - 2 * 60 * 60 * 1000);
  old.relay_created_at_ms = now;
  old.relay_server_time_ms = now;
  EXPECT_EQ(lane.Ingest(old).decision, IngestDecision::SilentDiscard);

  // Fresh in the relay, and a skewed sender clock within bounds: applies.
  auto skewed = lane.harness.MakeControlEnvelope("lane-skewed", TestControl("test_signal"), now - 10 * 60 * 1000);
  skewed.relay_created_at_ms = 5'000;
  skewed.relay_server_time_ms = 6'000;
  EXPECT_EQ(lane.Ingest(skewed).decision, IngestDecision::AcceptControl);
  EXPECT_EQ(family.applied, 1);
}

TEST(MessagingCrossCuttingTest, LaneCarriesOnlyLaneFamilies) {
  LaneHarness lane("lane_only_lane");
  TestFamily stream_family;
  lane.controls.Bind(stream_family.Make("billing", "test_billing", false, false));
  const int64_t now = util::NowUnixMs();
  EXPECT_EQ(lane.Ingest(lane.harness.MakeControlEnvelope("lane-billing", TestControl("test_billing"), now)).decision,
            IngestDecision::SilentDiscard);
  EXPECT_EQ(lane.Ingest(lane.harness.MakeControlEnvelope("lane-nobody", TestControl("test_unknown"), now)).decision,
            IngestDecision::SilentDiscard);
  EXPECT_EQ(stream_family.applied, 0);
}

TEST(MessagingCrossCuttingTest, LaneRejectsABadSignature) {
  LaneHarness lane("lane_forged");
  TestFamily family;
  lane.controls.Bind(family.Make("call", "test_signal", true, false));
  auto envelope = lane.harness.MakeControlEnvelope("lane-forged", TestControl("test_signal"), util::NowUnixMs());
  envelope.timestamp += 1;  // signed field changed after signing
  EXPECT_EQ(lane.Ingest(envelope).decision, IngestDecision::HardReject);
  EXPECT_EQ(family.applied, 0);
}

TEST(MessagingCrossCuttingTest, ControlFamilyFailureRefusesTheMessage) {
  PipelineHarness harness("control_refused", ThreadChannel::E2e);
  SystemControlRegistry controls;
  TestFamily family;
  family.fail = true;
  controls.Bind(family.Make("ping", "test_ping", false, false));
  harness.pipeline.SetSystemControls(&controls);

  const RelayReceiveOutcome outcome = harness.pipeline.ProcessEnvelope(
      harness.MakeSignedPayloadEnvelope("ctl-4", 1, TestControl("test_ping")), harness.local_account_id);
  EXPECT_EQ(outcome.decision, IngestDecision::HardReject);
  EXPECT_FALSE(outcome.persisted);
  ASSERT_TRUE(outcome.receive_failure_notice.has_value());
  EXPECT_NE(outcome.receive_failure_notice->find("couldn't apply ping update"), std::string::npos)
      << *outcome.receive_failure_notice;
  EXPECT_FALSE(harness.Stored("ctl-4").has_value());
}
