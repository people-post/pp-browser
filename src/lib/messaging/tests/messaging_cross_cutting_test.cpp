#include "foundation/crypto/CryptoUtil.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "lib/messaging/EnvelopeSigner.h"
#include "common/chat/MessagingLimits.h"
#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/messaging/RelayWirePayload.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/messaging/ReactionTypes.h"
#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/TranscriptAssembly.h"
#include "lib/messaging/ControlLaneEnvelope.h"
#include "lib/messaging/ControlLaneSeenStore.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/tests/TestAccount.h"
#include "lib/messaging/SqlitePskSessionStore.h"

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
        local(test::TestAccount::Make()),
        roster_store(store.ProfileDbPath()),
        psk_store(store.ProfileDbPath(), "test"),
        peer(test::TestAccount::Make()),
        pipeline(
            store,
            [this](const std::string& account) -> std::optional<AccountCard> {
              // The peer's device as its kept card lists it (filed under the fixture's readable id).
              return account == "account:peer" ? std::optional<AccountCard>(peer.DeviceCard()) : std::nullopt;
            },
            psk_store, local.Keys(), roster_store) {
    std::filesystem::remove_all(data_dir);
    if (!psk_store.SetDek(TestDek()) || !store.SetDek(TestDek())) {
      throw std::runtime_error("Failed to set test DEK");
    }


    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = "account:peer";
    target.channel = channel;

    auto created = store.FindOrCreateDirectThread(target, "contact-peer", "Peer");
    if (!created) {
      throw std::runtime_error("Failed to create thread");
    }
    thread = *created;

    local_account_id = local.account_id;

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
      local_kem_private = local.kem_private;
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
      auto local_public_b64 = Roe<std::string>(local.KemPublicB64());
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

    peer.SignEnvelope(envelope);
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
    peer.SignEnvelope(envelope);
    return envelope;
  }

  /**
   * The peer's control-lane envelope carrying `body`, sent at `sent_ms` — built by the product's
   * BuildControlLaneEnvelope, the peer opening the pair's session once (AutoKey to our KEM key).
   */
  RelayEnvelope MakeControlEnvelope(const std::string& message_id, const ThreadMessage& body, int64_t sent_ms) const {
    if (!lane_session) {
      auto local_public_b64 = Roe<std::string>(local.KemPublicB64());
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
    params.sender_device = peer.device.peer_id;
    ThreadMessage control = body;
    control.id = message_id;
    control.timestamp = sent_ms;
    auto envelope = BuildControlLaneEnvelope(
        params, control, [this](const std::vector<uint8_t>& bytes) { return peer.device.Sign(bytes); });
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
  test::TestAccount local;
  GroupRosterStore roster_store;
  SqlitePskSessionStore psk_store;
  /** The peer: its device signs what it sends. */
  test::TestAccount peer;
  RelayReceivePipeline pipeline;
  Thread thread;
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
  envelope.body.e2e.copies.front().payload_b64 = Base64Encode(huge);

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
  EXPECT_FALSE(outcome.receive_failure.has_value());
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

// --- a group's membership events travel as direct messages, but are not read in that chat ----------------------

constexpr const char* kHikeGroup = "group:hike";

// The group `peer` owns, with this device as a member, and its group thread.
Thread SeedPeerOwnedGroup(PipelineHarness& harness) {
  GroupMetadata metadata;
  metadata.group_id = kHikeGroup;
  metadata.owner_identity = "account:peer";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  EXPECT_TRUE(harness.roster_store.UpsertMetadata(metadata));
  for (const auto& [identity, role] : {std::pair<std::string, MemberRole>{"account:peer", MemberRole::Owner},
                                       {harness.local_account_id, MemberRole::Member},
                                       {"account:carol", MemberRole::Member}}) {
    GroupRosterMember member;
    member.member_identity = identity;
    member.role = role;
    member.joined_at = 1;
    EXPECT_TRUE(harness.roster_store.UpsertMember(kHikeGroup, member));
  }
  auto group = harness.store.FindOrCreateGroupThread(kHikeGroup, "Weekend hike", {});
  EXPECT_TRUE(group);
  return *group;
}

RelayEnvelope EventFromPeer(const PipelineHarness& harness, const GroupMembershipControlType type,
                            const std::string& detail, const uint64_t seq) {
  auto event = GroupMembershipCodec::BuildSystemMessage("", type, "event text", detail, "account:peer");
  EXPECT_TRUE(event);
  return harness.MakeSignedPayloadEnvelope("event-" + std::to_string(seq), seq, *event);
}

std::vector<TranscriptRow> Shown(PipelineHarness& harness, const std::string& thread_id, const bool direct) {
  auto page = harness.store.GetMessagesPage(thread_id, std::nullopt, 100);
  EXPECT_TRUE(page);
  return AssembleTranscript(page ? *page : std::vector<ThreadMessage>{}, nullptr, direct);
}

// A direct chat the event arrived in: stored, applied, and nowhere to be seen or counted.
void ExpectQuietInDirectChat(PipelineHarness& harness, const RelayReceiveOutcome& outcome) {
  EXPECT_TRUE(outcome.persisted);
  EXPECT_TRUE(outcome.suppress_inbox_chrome) << "no unread count, preview or notice";
  auto direct = harness.store.GetThread(harness.thread.id);
  ASSERT_TRUE(direct && *direct);
  EXPECT_TRUE((*direct)->preview.empty());
  EXPECT_EQ((*direct)->unread_count, 0);
  EXPECT_EQ((*direct)->updated_at, harness.thread.updated_at) << "the chat does not move up the list";
  auto stored = harness.store.GetMessagesPage(harness.thread.id, std::nullopt, 100);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->size(), 1u) << "the message is kept as it came (it carries the signed roster)";
  EXPECT_TRUE(Shown(harness, harness.thread.id, true).empty()) << "but not shown in the direct chat";
}

TEST(MessagingCrossCuttingTest, ARenameInTheDirectChatShowsOneLineInTheGroupAndNothingHere) {
  PipelineHarness harness("event_rename", ThreadChannel::E2e);
  const Thread group = SeedPeerOwnedGroup(harness);
  auto detail = GroupMembershipCodec::EncodeGroupRenamed(kHikeGroup, "Sunday hike", 2, 1);
  ASSERT_TRUE(detail);
  const auto outcome = harness.pipeline.ProcessEnvelope(
      EventFromPeer(harness, GroupMembershipControlType::GroupRenamed, *detail, 1), harness.local_account_id);
  ExpectQuietInDirectChat(harness, outcome);
  const auto lines = Shown(harness, group.id, false);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].message.text, "Group renamed to Sunday hike");
}

TEST(MessagingCrossCuttingTest, AKickInTheDirectChatShowsOneLineInTheGroupAndNothingHere) {
  PipelineHarness harness("event_kick", ThreadChannel::E2e);
  const Thread group = SeedPeerOwnedGroup(harness);
  auto detail = GroupMembershipCodec::EncodeMemberRemoved(kHikeGroup, "account:carol", 2);
  ASSERT_TRUE(detail);
  const auto outcome = harness.pipeline.ProcessEnvelope(
      EventFromPeer(harness, GroupMembershipControlType::MemberRemoved, *detail, 1), harness.local_account_id);
  ExpectQuietInDirectChat(harness, outcome);
  const auto lines = Shown(harness, group.id, false);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].message.text, "Member removed");
}

TEST(MessagingCrossCuttingTest, AJoinIsOneLineInTheGroupForAMemberAndNothingInTheDirectChat) {
  PipelineHarness harness("event_join", ThreadChannel::E2e);
  const Thread group = SeedPeerOwnedGroup(harness);
  auto detail = GroupMembershipCodec::EncodeMemberJoined(kHikeGroup, "account:dave", MemberRole::Member, 2);
  ASSERT_TRUE(detail);
  const auto outcome = harness.pipeline.ProcessEnvelope(
      EventFromPeer(harness, GroupMembershipControlType::MemberJoined, *detail, 1), harness.local_account_id);
  ExpectQuietInDirectChat(harness, outcome);
  const auto lines = Shown(harness, group.id, false);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].message.text, "Member joined the group");
}

TEST(MessagingCrossCuttingTest, TheNewcomerAlreadyWroteItsOwnJoinLineSoTheOwnersAnnouncementAddsNone) {
  PipelineHarness harness("event_join_self", ThreadChannel::E2e);
  const Thread group = SeedPeerOwnedGroup(harness);
  // What accepting wrote in the group thread (GroupMembershipWorkflow::AcceptInvite).
  auto own = GroupMembershipCodec::BuildSystemMessage(group.id, GroupMembershipControlType::MemberJoined,
                                                      "You joined the group", "{}", harness.local_account_id);
  ASSERT_TRUE(own);
  ASSERT_TRUE(harness.store.AppendMessage(*own));
  auto detail = GroupMembershipCodec::EncodeMemberJoined(kHikeGroup, harness.local_account_id, MemberRole::Member, 2);
  ASSERT_TRUE(detail);
  const auto outcome = harness.pipeline.ProcessEnvelope(
      EventFromPeer(harness, GroupMembershipControlType::MemberJoined, *detail, 1), harness.local_account_id);
  ExpectQuietInDirectChat(harness, outcome);
  const auto lines = Shown(harness, group.id, false);
  ASSERT_EQ(lines.size(), 1u) << "one join, one line";
  EXPECT_EQ(lines[0].message.text, "You joined the group");
}

TEST(MessagingCrossCuttingTest, AnAcceptInTheDirectChatShowsNothingThere) {
  PipelineHarness harness("event_accept", ThreadChannel::E2e);
  // This device owns the group and invited the peer.
  GroupMetadata metadata;
  metadata.group_id = kHikeGroup;
  metadata.owner_identity = harness.local_account_id;
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(harness.roster_store.UpsertMetadata(metadata));
  GroupRosterMember owner;
  owner.member_identity = harness.local_account_id;
  owner.role = MemberRole::Owner;
  owner.joined_at = 1;
  ASSERT_TRUE(harness.roster_store.UpsertMember(kHikeGroup, owner));
  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-1";
  pending.group_id = kHikeGroup;
  pending.group_title = "Weekend hike";
  pending.inviter_identity = harness.local_account_id;
  pending.invitee_identity = "account:peer";
  pending.roster_epoch = 1;
  pending.created_at = 1;
  pending.outbound = true;
  ASSERT_TRUE(harness.roster_store.UpsertPendingInvite(pending));
  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-1", kHikeGroup);
  ASSERT_TRUE(detail);
  const auto outcome = harness.pipeline.ProcessEnvelope(
      EventFromPeer(harness, GroupMembershipControlType::GroupInviteAccept, *detail, 1), harness.local_account_id);
  ExpectQuietInDirectChat(harness, outcome);
}

TEST(MessagingCrossCuttingTest, ADeclineLeavesALineInTheGroupForTheInviter) {
  PipelineHarness harness("event_decline", ThreadChannel::E2e);
  GroupMetadata metadata;
  metadata.group_id = kHikeGroup;
  metadata.owner_identity = harness.local_account_id;
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(harness.roster_store.UpsertMetadata(metadata));
  GroupRosterMember owner;
  owner.member_identity = harness.local_account_id;
  owner.role = MemberRole::Owner;
  owner.joined_at = 1;
  ASSERT_TRUE(harness.roster_store.UpsertMember(kHikeGroup, owner));
  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-2";
  pending.group_id = kHikeGroup;
  pending.group_title = "Weekend hike";
  pending.inviter_identity = harness.local_account_id;
  pending.invitee_identity = "account:peer";
  pending.roster_epoch = 1;
  pending.created_at = 1;
  pending.outbound = true;
  ASSERT_TRUE(harness.roster_store.UpsertPendingInvite(pending));
  auto group = harness.store.FindOrCreateGroupThread(kHikeGroup, "Weekend hike", {});
  ASSERT_TRUE(group);
  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-2", kHikeGroup);
  ASSERT_TRUE(detail);
  const auto outcome = harness.pipeline.ProcessEnvelope(
      EventFromPeer(harness, GroupMembershipControlType::GroupInviteDecline, *detail, 1), harness.local_account_id);
  ExpectQuietInDirectChat(harness, outcome);
  const auto lines = Shown(harness, group->id, false);
  ASSERT_EQ(lines.size(), 1u) << "hiding the direct message removed the only record: the group keeps one";
  EXPECT_EQ(lines[0].message.sender_contact_id, "account:peer");
  EXPECT_EQ(GroupMembershipCodec::ControlTypeFromMessage(lines[0].message),
            std::optional<GroupMembershipControlType>(GroupMembershipControlType::GroupInviteDecline));
  EXPECT_EQ(outcome.about_us, RelayReceiveOutcome::AboutUs::None) << "a decline is the inviter's news only in the group";
}

TEST(MessagingCrossCuttingTest, OnlyEventsAboutThisAccountAreNewsInTheGroup) {
  PipelineHarness harness("event_about_us", ThreadChannel::E2e);
  const Thread group = SeedPeerOwnedGroup(harness);
  const auto run = [&](const GroupMembershipControlType type, const std::string& detail, const uint64_t seq) {
    return harness.pipeline.ProcessEnvelope(EventFromPeer(harness, type, detail, seq), harness.local_account_id);
  };
  // Other people's events: quiet.
  EXPECT_EQ(run(GroupMembershipControlType::GroupRenamed, *GroupMembershipCodec::EncodeGroupRenamed(kHikeGroup, "X", 2, 1), 1)
                .about_us,
            RelayReceiveOutcome::AboutUs::None);
  EXPECT_EQ(run(GroupMembershipControlType::MemberJoined,
                *GroupMembershipCodec::EncodeMemberJoined(kHikeGroup, "account:dave", MemberRole::Member, 3), 2)
                .about_us,
            RelayReceiveOutcome::AboutUs::None);
  EXPECT_EQ(run(GroupMembershipControlType::MemberRemoved, *GroupMembershipCodec::EncodeMemberRemoved(kHikeGroup, "account:carol", 4), 3)
                .about_us,
            RelayReceiveOutcome::AboutUs::None);
  // This account removed: news in the GROUP thread.
  const auto removed = run(GroupMembershipControlType::MemberRemoved,
                           *GroupMembershipCodec::EncodeMemberRemoved(kHikeGroup, harness.local_account_id, 5), 4);
  EXPECT_EQ(removed.about_us, RelayReceiveOutcome::AboutUs::Removed);
  EXPECT_EQ(removed.about_us_group_thread_id, group.id);
  EXPECT_TRUE(removed.suppress_inbox_chrome) << "the direct message itself stays quiet";
}

TEST(MessagingCrossCuttingTest, AGroupFullRefusalIsNewsInTheGroup) {
  PipelineHarness harness("event_full", ThreadChannel::E2e);
  GroupMetadata metadata;
  metadata.group_id = kHikeGroup;
  metadata.owner_identity = "account:peer";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(harness.roster_store.UpsertMetadata(metadata));
  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-3";
  pending.group_id = kHikeGroup;
  pending.group_title = "Weekend hike";
  pending.inviter_identity = "account:peer";
  pending.invitee_identity = harness.local_account_id;
  pending.roster_epoch = 1;
  pending.created_at = 1;
  pending.outbound = false;
  ASSERT_TRUE(harness.roster_store.UpsertPendingInvite(pending));
  auto group = harness.store.FindOrCreateGroupThread(kHikeGroup, "Weekend hike", {});
  ASSERT_TRUE(group);
  auto detail = GroupMembershipCodec::EncodeInviteRefused("nonce-3", kHikeGroup, "group_full");
  ASSERT_TRUE(detail);
  const auto outcome = harness.pipeline.ProcessEnvelope(
      EventFromPeer(harness, GroupMembershipControlType::GroupInviteRefused, *detail, 1), harness.local_account_id);
  EXPECT_TRUE(outcome.persisted);
  EXPECT_EQ(outcome.about_us, RelayReceiveOutcome::AboutUs::GroupFull);
  EXPECT_EQ(outcome.about_us_group_thread_id, group->id);
}

TEST(MessagingCrossCuttingTest, AGroupInvitationStillShowsAndCountsInTheDirectChat) {
  PipelineHarness harness("event_invite", ThreadChannel::E2e);
  GroupInvitePayload invite;
  invite.group_id = kHikeGroup;
  invite.group_title = "Weekend hike";
  invite.inviter_identity = "account:peer";
  invite.invitee_identity = harness.local_account_id;
  invite.invite_nonce = "nonce-9";
  invite.roster_epoch = 1;
  auto detail = GroupMembershipCodec::EncodeInvite(invite);
  ASSERT_TRUE(detail);
  const auto outcome = harness.pipeline.ProcessEnvelope(
      EventFromPeer(harness, GroupMembershipControlType::GroupInvite, *detail, 1), harness.local_account_id);
  EXPECT_TRUE(outcome.persisted);
  EXPECT_FALSE(outcome.suppress_inbox_chrome);
  EXPECT_EQ(Shown(harness, harness.thread.id, true).size(), 1u);
}

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
  ASSERT_TRUE(outcome.receive_failure.has_value());
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::ApplyFailed);
  EXPECT_EQ(outcome.receive_failure_family, "ping");
  EXPECT_FALSE(harness.Stored("ctl-4").has_value());
}
