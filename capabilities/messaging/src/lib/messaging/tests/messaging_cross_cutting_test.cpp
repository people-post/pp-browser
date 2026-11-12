#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/E2eRelayPayloadCodec.h"
#include "lib/delivery/EnvelopeSigner.h"
#include "lib/delivery/OutboundMessage.h"
#include "common/chat/MessagingLimits.h"
#include "lib/delivery/AutoKeyEstablishment.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/delivery/RelayWirePayload.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/delivery/AutoKeyEstablishment.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/messaging/TranscriptAssembly.h"
#include "lib/delivery/ControlLaneEnvelope.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/EditMessage.h"
#include "lib/messaging/tests/TestAccount.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <tuple>
#include <vector>

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
  explicit PipelineHarness(const std::string& suffix, const ThreadChannel channel)
      : data_dir(std::filesystem::temp_directory_path() / ("pp_browser_cross_cutting_" + suffix)),
        store(),
        local(test::TestAccount::Make()),
        roster_store(store),
        psk_store(store),
        peer(test::TestAccount::Make()),
        pipeline(
            store,
            [this](const std::string& account) -> std::optional<AccountCard> {
              // The peer's device as its kept card lists it (filed under the fixture's readable id).
              return account == "account:peer" ? std::optional<AccountCard>(peer.DeviceCard()) : std::nullopt;
            },
            psk_store, test::ReceiveKeysOf(local), roster_store) {
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
      PeerSessionRecord psk;
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
                                   const ThreadChannel channel, std::optional<int64_t> timestamp = std::nullopt) const {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = message_id;
    envelope.sender_contact_id = "account:peer";
    envelope.sender_relay_id = "relay:peer";
    envelope.route.kind = "direct";
    envelope.route.channel = channel;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = timestamp.value_or(static_cast<int64_t>(seq));

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
      auto payload =
          E2eRelayPayloadCodec::EncryptTextWithAutoKey(params, established->master_psk, established->key_init_b64);
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

    test::SignEnvelopeAs(peer, envelope);
    return envelope;
  }

  /** A peer E2E envelope carrying any chat payload (annotations etc.), signed like the text one. */
  RelayEnvelope MakeSignedPayloadEnvelope(const std::string& message_id, uint64_t seq, const ThreadMessage& body,
                                          std::optional<int64_t> timestamp = std::nullopt) const {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = message_id;
    envelope.sender_contact_id = "account:peer";
    envelope.sender_relay_id = "relay:peer";
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2e;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = timestamp.value_or(static_cast<int64_t>(seq));
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
    test::SignEnvelopeAs(peer, envelope);
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
    params.copies = {ControlLaneCopy{
        .session_epoch = 1, .master_psk = lane_session->master_psk, .key_init_b64 = lane_session->key_init_b64}};
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

  /** The peer's edit of `target_id`, sent at `sent_at`. */
  RelayEnvelope MakeEdit(const std::string& message_id, uint64_t seq, const std::string& target_id,
                         const std::string& text, int64_t sent_at) const {
    ThreadMessage edit;
    edit.content_type = ChatContentType::Annotation;
    edit.target_message_id = target_id;
    edit.text = text;
    edit.payload_json = BuildEditPayloadJson(target_id, text);
    return MakeSignedPayloadEnvelope(message_id, seq, edit, sent_at);
  }

  /** Every stored row that still carries `words` anywhere in its text or payload. */
  size_t RowsHolding(const std::string& words) {
    size_t n = 0;
    const std::vector<ThreadMessage> rows = store.GetMessages(thread.id).value();
    for (const ThreadMessage& m : rows) {
      n += m.text.find(words) != std::string::npos || m.payload_json.find(words) != std::string::npos ? 1 : 0;
    }
    return n;
  }

  /** Edit rows aimed at `target_id` that still carry text. */
  size_t EditRowsWithText(const std::string& target_id) {
    size_t n = 0;
    const std::vector<ThreadMessage> rows = store.GetAnnotationsForTarget(thread.id, target_id).value();
    for (const ThreadMessage& m : rows) {
      n += IsEditAnnotation(m) && !EditTextOf(m).value_or("").empty() ? 1 : 0;
    }
    return n;
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
  InMemoryDeliveryStore store;
  test::TestAccount local;
  InMemoryGroupRosterStore roster_store;
  InMemoryPeerSessions psk_store;
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
  const std::filesystem::path data_dir = std::filesystem::temp_directory_path() / "pp_browser_cross_cutting_routing";
  std::filesystem::remove_all(data_dir);
  InMemoryDeliveryStore store;
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
  explicit LaneHarness(const std::string& suffix) : harness(suffix, ThreadChannel::E2ePublic), seen() {
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
  InMemoryControlLaneSeenStore seen;
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
  return AssembleTranscript(page ? *page : std::vector<ThreadMessage>{}, nullptr,
                            direct ? ThreadKind::Direct : ThreadKind::Group);
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
  EXPECT_EQ(outcome.about_us, RelayReceiveOutcome::AboutUs::None)
      << "a decline is the inviter's news only in the group";
}

TEST(MessagingCrossCuttingTest, OnlyEventsAboutThisAccountAreNewsInTheGroup) {
  PipelineHarness harness("event_about_us", ThreadChannel::E2e);
  const Thread group = SeedPeerOwnedGroup(harness);
  const auto run = [&](const GroupMembershipControlType type, const std::string& detail, const uint64_t seq) {
    return harness.pipeline.ProcessEnvelope(EventFromPeer(harness, type, detail, seq), harness.local_account_id);
  };
  // Other people's events: quiet.
  EXPECT_EQ(
      run(GroupMembershipControlType::GroupRenamed, *GroupMembershipCodec::EncodeGroupRenamed(kHikeGroup, "X", 2, 1), 1)
          .about_us,
      RelayReceiveOutcome::AboutUs::None);
  EXPECT_EQ(run(GroupMembershipControlType::MemberJoined,
                *GroupMembershipCodec::EncodeMemberJoined(kHikeGroup, "account:dave", MemberRole::Member, 3), 2)
                .about_us,
            RelayReceiveOutcome::AboutUs::None);
  EXPECT_EQ(run(GroupMembershipControlType::MemberRemoved,
                *GroupMembershipCodec::EncodeMemberRemoved(kHikeGroup, "account:carol", 4), 3)
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

  const RelayReceiveOutcome again = lane.Ingest(envelope); // relay redelivery / second transport
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
  envelope.timestamp += 1; // signed field changed after signing
  EXPECT_EQ(lane.Ingest(envelope).decision, IngestDecision::HardReject);
  EXPECT_EQ(family.applied, 0);
}

// Lane messages carry the sender's card only when the peer may lack it: one from a device the kept card does not
// list cannot be checked, so the receiver asks that device for the card (and the message itself is refused).
TEST(MessagingCrossCuttingTest, LaneFromADeviceNotOnTheKeptCardAsksForTheCard) {
  LaneHarness lane("lane_unknown_device");
  TestFamily family;
  lane.controls.Bind(family.Make("call", "test_signal", true, false));
  std::vector<std::tuple<std::string, std::string, int64_t>> asked;
  lane.harness.pipeline.SetOnUnknownSenderDevice(
      [&](const std::string& account, const std::string& device, const int64_t issued_at) {
        asked.emplace_back(account, device, issued_at);
      });

  const test::TestDevice other = test::NamedDevice("lane-other-device");
  auto envelope = lane.harness.MakeControlEnvelope("lane-unknown", TestControl("test_signal"), util::NowUnixMs());
  envelope.sender_device = other.peer_id;
  envelope.body.e2e.sender_card_at = 5; // newer than the kept card (issued_at 1)
  auto bytes = EnvelopeSigner::BuildSignBytes(envelope);
  ASSERT_TRUE(bytes);
  auto signature = other.Sign(*bytes);
  ASSERT_TRUE(signature);
  envelope.signature = *signature;

  EXPECT_NE(lane.Ingest(envelope).decision, IngestDecision::AcceptControl);
  EXPECT_EQ(family.applied, 0);
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(std::get<0>(asked[0]), "account:peer");
  EXPECT_EQ(std::get<1>(asked[0]), other.peer_id);
  EXPECT_EQ(std::get<2>(asked[0]), 5);

  // No newer card claimed: nothing to ask for (a device the sender's own newest list leaves out).
  envelope.body.e2e.sender_card_at = 1;
  (void)lane.Ingest(envelope);
  EXPECT_EQ(asked.size(), 1u);
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

// ---- Editing a sent message (DEV/PLAN-message-edit.md) ----

namespace {

constexpr int64_t kSent = 1'000'000;
constexpr int64_t kMin = 60'000;

/** The peer's text "see you at 8", seq 1, sent at kSent. */
void ReceiveOriginal(PipelineHarness& harness, const std::string& id = "peer-1", uint64_t seq = 1,
                     bool backfill = false) {
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeSignedEnvelope(id, seq, "see you at 8", ThreadChannel::E2e, kSent),
                                   harness.local_account_id, backfill)
                  .persisted);
}

} // namespace

// The author's edit rewrites the row in place: new text, the mark, same id / seq / order / send time; quietly.
TEST(MessagingCrossCuttingTest, TheAuthorsEditRewritesTheMessageInPlace) {
  PipelineHarness harness("edit_applies", ThreadChannel::E2e);
  std::vector<std::pair<std::string, std::string>> edited;
  harness.pipeline.SetOnMessageEdited([&](const std::string&, const ThreadMessage& before, const ThreadMessage& after) {
    edited.emplace_back(before.text, after.text);
  });
  ReceiveOriginal(harness);
  const ThreadMessage before = *harness.Stored("peer-1");

  const RelayReceiveOutcome outcome = harness.pipeline.ProcessEnvelope(
      harness.MakeEdit("edit-1", 2, "peer-1", "see you at 9", kSent + 2 * kMin), harness.local_account_id);
  EXPECT_TRUE(outcome.persisted);
  EXPECT_TRUE(outcome.suppress_inbox_chrome) << "no unread count, preview or notice";
  EXPECT_FALSE(outcome.deleted_by_sender);
  const ThreadMessage after = *harness.Stored("peer-1");
  EXPECT_EQ(after.text, "see you at 9");
  EXPECT_EQ(after.content_type, ChatContentType::Text);
  ASSERT_TRUE(EditMarkOf(after).has_value());
  EXPECT_EQ(EditMarkOf(after)->edited_at, kSent + 2 * kMin);
  EXPECT_EQ(EditMarkOf(after)->edited_by, "edit-1");
  EXPECT_EQ(after.id, before.id);
  EXPECT_EQ(after.sender_seq, before.sender_seq);
  EXPECT_EQ(after.display_order, before.display_order);
  EXPECT_EQ(after.timestamp, before.timestamp);
  ASSERT_EQ(edited.size(), 1u);
  EXPECT_EQ(edited[0], std::make_pair(std::string("see you at 8"), std::string("see you at 9")));
}

// Only the author: a peer's edit aimed at our message changes nothing (and its words are not kept).
TEST(MessagingCrossCuttingTest, AnEditOfSomeoneElsesMessageIsIgnored) {
  PipelineHarness harness("edit_not_author", ThreadChannel::E2e);
  ThreadMessage mine;
  mine.id = "mine-1";
  mine.thread_id = harness.thread.id;
  mine.sender_contact_id = kLocalSelfContactId;
  mine.text = "keep me";
  mine.timestamp = kSent;
  ASSERT_TRUE(harness.store.AppendMessage(mine));
  const RelayReceiveOutcome outcome = harness.pipeline.ProcessEnvelope(
      harness.MakeEdit("edit-1", 1, "mine-1", "forged words", kSent + kMin), harness.local_account_id);
  EXPECT_TRUE(outcome.persisted);
  EXPECT_EQ(harness.Stored("mine-1")->text, "keep me");
  EXPECT_FALSE(EditMarkOf(*harness.Stored("mine-1")).has_value());
  EXPECT_EQ(harness.RowsHolding("forged words"), 0u);
}

// The edit overtakes its message: judged and applied when the message arrives.
TEST(MessagingCrossCuttingTest, AnEditThatArrivesFirstIsAppliedWhenTheMessageComes) {
  PipelineHarness harness("edit_first", ThreadChannel::E2e);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-1", 2, "peer-1", "see you at 9", kSent + kMin),
                                   harness.local_account_id)
                  .persisted);
  ReceiveOriginal(harness, "peer-1", 1, /*backfill=*/true);
  const ThreadMessage after = *harness.Stored("peer-1");
  EXPECT_EQ(after.text, "see you at 9");
  EXPECT_EQ(EditMarkOf(after)->edited_by, "edit-1");
  EXPECT_EQ(harness.EditRowsWithText("peer-1"), 1u);
}

// A waiting edit from anyone but the message's sender is dropped when the message comes.
TEST(MessagingCrossCuttingTest, AWaitingEditFromSomeoneElseIsNotAppliedWhenTheMessageComes) {
  PipelineHarness harness("edit_first_not_author", ThreadChannel::E2e);
  ThreadMessage forged;
  forged.id = "edit-x";
  forged.thread_id = harness.thread.id;
  forged.sender_contact_id = "account:mallory";
  forged.content_type = ChatContentType::Annotation;
  forged.target_message_id = "peer-1";
  forged.text = "forged words";
  forged.payload_json = BuildEditPayloadJson("peer-1", "forged words");
  forged.timestamp = kSent + kMin;
  ASSERT_TRUE(harness.store.AppendMessage(forged));
  ReceiveOriginal(harness);
  EXPECT_EQ(harness.Stored("peer-1")->text, "see you at 8");
  EXPECT_EQ(harness.RowsHolding("forged words"), 0u) << "judged, then blanked";
}

// A waiting edit sent too long after the message is dropped when the message comes.
TEST(MessagingCrossCuttingTest, AWaitingEditSentTooLateIsNotAppliedWhenTheMessageComes) {
  PipelineHarness harness("edit_first_late", ThreadChannel::E2e);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-1", 2, "peer-1", "late words", kSent + 16 * kMin + 1),
                                   harness.local_account_id)
                  .persisted);
  ReceiveOriginal(harness, "peer-1", 1, /*backfill=*/true);
  EXPECT_EQ(harness.Stored("peer-1")->text, "see you at 8");
  EXPECT_EQ(harness.RowsHolding("late words"), 0u);
}

// A waiting edit aimed at what turns out to be a picture is dropped when it comes.
TEST(MessagingCrossCuttingTest, AWaitingEditOfAPictureIsNotApplied) {
  PipelineHarness harness("edit_first_picture", ThreadChannel::E2e);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-1", 2, "pic-1", "caption words", kSent + kMin),
                                   harness.local_account_id)
                  .persisted);
  ChatAttachmentFields fields;
  fields.url = "https://cdn.example/blobs/abc";
  fields.mime = "image/png";
  fields.filename = "diagram.png";
  fields.byte_length = 4096;
  fields.content_hash.assign(kAttachmentContentHashSize, 0x11);
  fields.content_key.assign(kSessionKeySize, 0x22);
  fields.blob_nonce.assign(kAeadNonceSize, 0x33);
  ThreadMessage picture;
  picture.content_type = ChatContentType::Attachment;
  picture.text = "diagram.png";
  picture.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeSignedPayloadEnvelope("pic-1", 1, picture, kSent),
                                   harness.local_account_id, true)
                  .persisted);
  const ThreadMessage stored = *harness.Stored("pic-1");
  EXPECT_EQ(stored.content_type, ChatContentType::Attachment);
  EXPECT_EQ(stored.text, "diagram.png");
  EXPECT_EQ(harness.RowsHolding("caption words"), 0u);
}

// Out of order: the edit sent later wins, whichever arrives last.
TEST(MessagingCrossCuttingTest, TheLaterSentEditWinsWhicheverArrivesLast) {
  PipelineHarness harness("edit_order", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(
      harness.pipeline
          .ProcessEnvelope(harness.MakeEdit("edit-b", 2, "peer-1", "ten", kSent + 3 * kMin), harness.local_account_id)
          .persisted);
  ASSERT_TRUE(
      harness.pipeline
          .ProcessEnvelope(harness.MakeEdit("edit-a", 3, "peer-1", "nine", kSent + 2 * kMin), harness.local_account_id)
          .persisted);
  EXPECT_EQ(harness.Stored("peer-1")->text, "ten");
  EXPECT_EQ(harness.RowsHolding("nine"), 0u);
  EXPECT_EQ(harness.EditRowsWithText("peer-1"), 1u);
}

// Order is by send time, never by seq: device A's edit (larger seq, earlier) loses to device B's (smaller seq, later).
TEST(MessagingCrossCuttingTest, AnEditFromAnotherDeviceWithASmallerSeqButLaterTimeWins) {
  PipelineHarness harness("edit_devices", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-a", 9, "peer-1", "from the phone", kSent + 2 * kMin),
                                   harness.local_account_id, true)
                  .persisted);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-b", 4, "peer-1", "from the mac", kSent + 4 * kMin),
                                   harness.local_account_id, true)
                  .persisted);
  EXPECT_EQ(harness.Stored("peer-1")->text, "from the mac");
}

// Same send time: the larger message id wins (every device decides the same).
TEST(MessagingCrossCuttingTest, AtTheSameSendTimeTheLargerIdWins) {
  PipelineHarness harness("edit_tie", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(
      harness.pipeline
          .ProcessEnvelope(harness.MakeEdit("edit-z", 2, "peer-1", "zed", kSent + kMin), harness.local_account_id)
          .persisted);
  ASSERT_TRUE(
      harness.pipeline
          .ProcessEnvelope(harness.MakeEdit("edit-a", 3, "peer-1", "ay", kSent + kMin), harness.local_account_id)
          .persisted);
  EXPECT_EQ(harness.Stored("peer-1")->text, "zed");
}

// Device B's clock 3 minutes slow (then 3 fast): A edits first, B edits after receiving it — B's edit wins everywhere,
// because B stamps it after the edit it already holds (EditSendTime).
TEST(MessagingCrossCuttingTest, TheLaterEditWinsWhateverTheSecondDevicesClockSays) {
  for (const int64_t skew : {-3 * kMin, 3 * kMin}) {
    PipelineHarness harness(skew < 0 ? "edit_clock_slow" : "edit_clock_fast", ThreadChannel::E2e);
    ReceiveOriginal(harness);
    ThreadMessage on_b = *harness.Stored("peer-1"); // device B holds the same message
    ThreadMessage edit_a;
    edit_a.id = "edit-a";
    edit_a.sender_contact_id = on_b.sender_contact_id;
    edit_a.content_type = ChatContentType::Annotation;
    edit_a.target_message_id = "peer-1";
    edit_a.payload_json = BuildEditPayloadJson("peer-1", "from A");
    edit_a.timestamp = kSent + 4 * kMin;
    ASSERT_TRUE(harness.pipeline
                    .ProcessEnvelope(harness.MakeEdit("edit-a", 2, "peer-1", "from A", edit_a.timestamp),
                                     harness.local_account_id)
                    .persisted);
    on_b = WithEdit(on_b, edit_a); // B received A's edit
    const int64_t b_sends_at = EditSendTime(kSent + 5 * kMin + skew, on_b);
    ASSERT_TRUE(
        harness.pipeline
            .ProcessEnvelope(harness.MakeEdit("edit-0", 3, "peer-1", "from B", b_sends_at), harness.local_account_id)
            .persisted);
    EXPECT_EQ(harness.Stored("peer-1")->text, "from B") << "skew " << skew;
  }
}

// Tapped Send at minute 14, sent 20 minutes later (offline): the send time is the tap's, so it still counts.
TEST(MessagingCrossCuttingTest, AnEditTappedInTimeButDeliveredLateStillCounts) {
  PipelineHarness harness("edit_offline", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-1", 2, "peer-1", "see you at 9", kSent + 14 * kMin),
                                   harness.local_account_id)
                  .persisted);
  EXPECT_EQ(harness.Stored("peer-1")->text, "see you at 9");
}

// Sent more than 15 + 1 minutes after the message: ignored, its words not kept.
TEST(MessagingCrossCuttingTest, AnEditSentTooLateIsIgnored) {
  PipelineHarness harness("edit_late", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-1", 2, "peer-1", "late words", kSent + 16 * kMin + 1),
                                   harness.local_account_id)
                  .persisted);
  EXPECT_EQ(harness.Stored("peer-1")->text, "see you at 8");
  EXPECT_EQ(harness.RowsHolding("late words"), 0u);
}

// Stamped before the message it edits: ignored, its words not kept.
TEST(MessagingCrossCuttingTest, AnEditStampedBeforeItsMessageIsIgnored) {
  PipelineHarness harness("edit_early", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-1", 2, "peer-1", "early words", kSent - kMin),
                                   harness.local_account_id)
                  .persisted);
  EXPECT_EQ(harness.Stored("peer-1")->text, "see you at 8");
  EXPECT_EQ(harness.RowsHolding("early words"), 0u);
}

// No text, or too long: ignored.
TEST(MessagingCrossCuttingTest, AnEmptyOrOverlongEditIsIgnored) {
  PipelineHarness harness("edit_text_limits", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  (void)harness.pipeline.ProcessEnvelope(harness.MakeEdit("edit-1", 2, "peer-1", "", kSent + kMin),
                                         harness.local_account_id);
  // Too long: refused before it is stored (a payload over the limit does not even decrypt as one).
  try {
    (void)harness.pipeline.ProcessEnvelope(
        harness.MakeEdit("edit-2", 3, "peer-1", std::string(kMaxEditTextBytes + 1, 'a'), kSent + kMin),
        harness.local_account_id);
  } catch (const std::runtime_error&) {
    // the harness could not even seal it
  }
  EXPECT_EQ(harness.Stored("peer-1")->text, "see you at 8");
  EXPECT_FALSE(EditMarkOf(*harness.Stored("peer-1")).has_value());
}

// An edit of a picture or of a deleted message: ignored; the late one after the delete leaves no words behind.
TEST(MessagingCrossCuttingTest, AnEditOfAPictureOrADeletedMessageIsIgnored) {
  PipelineHarness harness("edit_deleted", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(harness.pipeline.ProcessEnvelope(harness.MakeRetract("retract-1", 2, "peer-1"), harness.local_account_id)
                  .persisted);
  (void)harness.pipeline.ProcessEnvelope(harness.MakeEdit("edit-1", 3, "peer-1", "after the delete", kSent + kMin),
                                         harness.local_account_id);
  EXPECT_EQ(harness.Stored("peer-1")->content_type, ChatContentType::Deleted);
  EXPECT_EQ(harness.RowsHolding("after the delete"), 0u) << "an edit after the delete is not stored with its words";
}

// One edit applied leaves exactly one edit row with words: the applied one (its words are the message's).
TEST(MessagingCrossCuttingTest, OnlyTheAppliedEditKeepsItsWords) {
  PipelineHarness harness("edit_one_row", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  for (const auto& [id, seq, text, minute] : std::vector<std::tuple<std::string, uint64_t, std::string, int64_t>>{
           {"edit-1", 2, "first try", 1}, {"edit-2", 3, "second try", 2}, {"edit-3", 4, "third try", 3}}) {
    ASSERT_TRUE(
        harness.pipeline
            .ProcessEnvelope(harness.MakeEdit(id, seq, "peer-1", text, kSent + minute * kMin), harness.local_account_id)
            .persisted);
  }
  EXPECT_EQ(harness.Stored("peer-1")->text, "third try");
  EXPECT_EQ(harness.EditRowsWithText("peer-1"), 1u);
  EXPECT_EQ(harness.RowsHolding("first try"), 0u);
  EXPECT_EQ(harness.RowsHolding("second try"), 0u);
}

// Edited twice, then deleted for everyone: no row holds any of the edited words, and the message is a tombstone.
TEST(MessagingCrossCuttingTest, AnEditedMessageDeletedForEveryoneLeavesNoEditedWords) {
  PipelineHarness harness("edit_then_delete", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(
      harness.pipeline
          .ProcessEnvelope(harness.MakeEdit("edit-1", 2, "peer-1", "first try", kSent + kMin), harness.local_account_id)
          .persisted);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-2", 3, "peer-1", "second try", kSent + 2 * kMin),
                                   harness.local_account_id)
                  .persisted);
  ASSERT_TRUE(harness.pipeline.ProcessEnvelope(harness.MakeRetract("retract-1", 4, "peer-1"), harness.local_account_id)
                  .persisted);
  EXPECT_EQ(harness.Stored("peer-1")->content_type, ChatContentType::Deleted);
  EXPECT_EQ(harness.EditRowsWithText("peer-1"), 0u);
  EXPECT_EQ(harness.RowsHolding("first try"), 0u);
  EXPECT_EQ(harness.RowsHolding("second try"), 0u);
}

// An edit row is never shown: the chat shows the message once, with the new text.
TEST(MessagingCrossCuttingTest, AnEditRowIsNeverAChatRow) {
  PipelineHarness harness("edit_hidden", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  ASSERT_TRUE(harness.pipeline
                  .ProcessEnvelope(harness.MakeEdit("edit-1", 2, "peer-1", "see you at 9", kSent + kMin),
                                   harness.local_account_id)
                  .persisted);
  const auto rows =
      AssembleTranscript(harness.store.GetMessages(harness.thread.id).value(), nullptr, ThreadKind::Direct);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].message.id, "peer-1");
  EXPECT_EQ(rows[0].message.text, "see you at 9");
  EXPECT_TRUE(rows[0].annotation_badges.empty());
}

// The reaction cap counts reactions: a message edited 40 times (40 edit rows, all but one blanked) still takes ours and
// the peer's, and 32 reactions still fill it.
TEST(MessagingCrossCuttingTest, AMessageEditedFortyTimesStillTakesReactions) {
  PipelineHarness harness("edit_reaction_cap", ThreadChannel::E2e);
  ReceiveOriginal(harness);
  for (int i = 1; i <= 40; ++i) {
    ASSERT_TRUE(harness.pipeline
                    .ProcessEnvelope(harness.MakeEdit("edit-" + std::to_string(i), 1 + i, "peer-1",
                                                      "try " + std::to_string(i), kSent + i * 10'000),
                                     harness.local_account_id)
                    .persisted);
  }
  ASSERT_EQ(harness.Stored("peer-1")->text, "try 40");
  ASSERT_GE(harness.store.CountAnnotationsForTarget(harness.thread.id, "peer-1").value(), 40);

  SendRelayOptions ours;
  ours.content_type = ChatContentType::Annotation;
  ours.payload_json = BuildReactionPayloadJson(kAnnotationTypeReaction, "peer-1", "x");
  EXPECT_TRUE(
      AppendOutboundMessage(harness.store, NewOutboundMessage(harness.thread.id, "x", ours, "mine-react", kSent)))
      << "our reaction is not refused as too many";

  ThreadMessage reaction;
  reaction.content_type = ChatContentType::Annotation;
  reaction.target_message_id = "peer-1";
  reaction.text = "y";
  reaction.payload_json = BuildReactionPayloadJson(kAnnotationTypeReaction, "peer-1", "y");
  const RelayReceiveOutcome theirs = harness.pipeline.ProcessEnvelope(
      harness.MakeSignedPayloadEnvelope("peer-react", 42, reaction), harness.local_account_id);
  EXPECT_TRUE(theirs.persisted);
  EXPECT_FALSE(theirs.receive_failure.has_value()) << "the peer's reaction is not rejected";

  // Reactions alone still fill it: 30 more (32 with the two above), then the next one is refused, ours and theirs.
  for (int i = 0; i < 30; ++i) {
    ThreadMessage filler = reaction;
    filler.id = "fill-" + std::to_string(i);
    filler.thread_id = harness.thread.id;
    filler.sender_contact_id = "account:peer";
    filler.timestamp = kSent;
    ASSERT_TRUE(harness.store.AppendMessage(filler));
  }
  auto over = AppendOutboundMessage(harness.store, NewOutboundMessage(harness.thread.id, "x", ours, "over", kSent));
  ASSERT_FALSE(over);
  EXPECT_EQ(over.error().message, "Too many reactions on this message");
  const RelayReceiveOutcome flood = harness.pipeline.ProcessEnvelope(
      harness.MakeSignedPayloadEnvelope("peer-react-over", 43, reaction), harness.local_account_id);
  EXPECT_FALSE(flood.persisted);
  ASSERT_TRUE(flood.receive_failure.has_value());
  EXPECT_EQ(*flood.receive_failure, ReceiveFailure::ReactionRejected);
}
