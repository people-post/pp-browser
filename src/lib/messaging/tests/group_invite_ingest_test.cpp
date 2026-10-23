#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/E2eIngestClassifier.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "lib/messaging/EnvelopeSigner.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/PeerSigningKeyStore.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "common/directory/IContactTrustAccess.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/messaging/GroupInviteGate.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/tests/TestAccount.h"
#include "lib/messaging/SqlitePskSessionStore.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <set>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

ByteVector TestMasterPsk() {
  const auto bytes = HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  EXPECT_TRUE(bytes);
  return *bytes;
}

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xb0 + i);
  }
  return dek;
}

/** The device's contacts as messaging asks about them (people's): who is blocked, who is a contact. */
struct FakeContacts : IContactTrustAccess {
  std::set<std::string> known;
  std::set<std::string> blocked;

  Roe<bool> IsAccountBlocked(const std::string& account_id) const override { return blocked.count(account_id) > 0; }
  Roe<void> BlockAccountIfPresent(const std::string& account_id) override {
    if (known.count(account_id) > 0) {
      blocked.insert(account_id);
    }
    return {};
  }
  GroupInviteGate::StandingFn Standing() const {
    return [this](const std::string& identity) {
      if (blocked.count(identity) > 0) {
        return GroupInviteGate::InviterStanding::Blocked;
      }
      return known.count(identity) > 0 ? GroupInviteGate::InviterStanding::Contact
                                       : GroupInviteGate::InviterStanding::Unknown;
    };
  }
};

// Temp-dir cleanup (Windows CI): do not call remove_all while SqliteThreadStore /
// GroupRosterStore / SqlitePskSessionStore still hold open DB handles. Member
// destructors close SQLite first; a harness destructor that remove_all's first
// throws on Windows ("being used by another process") and aborts the test process.
// Use a unique path and skip destructor cleanup (same as SyncTestHarness).
struct PartyHarness {
  explicit PartyHarness(const std::string& suffix, const std::string& local_account, const std::string& peer_account,
                        const ThreadChannel channel = ThreadChannel::E2e)
      : data_dir(std::filesystem::temp_directory_path() /
                 ("pp_invite_ingest_" + suffix + "_" + util::GenerateUuid())),
        store(data_dir.string()), keys(test::TestAccount::Make()), roster(store.ProfileDbPath()),
        psk_store(store.ProfileDbPath(), "test"), key_resolver(key_store), gate(contacts.Standing(), roster),
        pipeline(store, key_resolver, psk_store,
                 LocalKeys{[local_account]() -> Roe<std::string> { return local_account; },
                           [kem = keys.kem_private]() -> Roe<ByteVector> { return kem; }},
                 roster, &gate),
        local_account(local_account), peer_account(peer_account),
        local_relay(RelayForAccount(local_account)), peer_relay(RelayForAccount(peer_account)), channel(channel) {
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);
    if (!psk_store.SetDek(TestDek()) || !store.SetDek(TestDek())) {
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
    key_store.PutUncheckedForTest("account", peer_account, record);

    contacts.known.insert(peer_account); // a friendly contact
    gate.SetInboundPolicy(GroupInvitePolicy::ContactsOnly);

    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = peer_account;
    target.channel = channel;
    auto created = store.FindOrCreateDirectThread(target, "contact-peer", peer_account);
    if (!created) {
      throw std::runtime_error("Failed to create DM");
    }
    dm_thread = *created;

    PskSessionRecord psk;
    psk.key = E2eRelayPayloadCodec::ChatTargetFromThread(dm_thread);
    psk.session_epoch = 1;
    psk.master_psk_b64 = Base64Encode(TestMasterPsk());
    psk.psk_verified_at = 1;
    if (!psk_store.Save(psk)) {
      throw std::runtime_error("Failed to save PSK");
    }
  }

  static std::string RelayForAccount(const std::string& account_id) {
    if (account_id.rfind("account:", 0) == 0) {
      return "relay:" + account_id.substr(std::string("account:").size());
    }
    return account_id;
  }

  RelayEnvelope MakeSystemEnvelope(const std::string& control_type, const std::string& detail,
                                   const std::string& text, const uint64_t seq) const {
    ThreadMessage system;
    system.content_type = ChatContentType::System;
    system.text = text;
    system.payload_json = [&] {
      Object payload;
      payload.set("control_type", control_type);
      payload.set("detail", detail);
      return DumpJson(payload);
    }();
    auto plaintext = ChatPayloadCodec::EncodeToRow(system);
    if (!plaintext) {
      throw std::runtime_error("Failed to encode system payload");
    }

    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = util::GenerateUuid();
    envelope.sender_contact_id = peer_account;
    envelope.sender_relay_id = peer_relay;
    envelope.route.kind = "direct";
    envelope.route.channel = channel;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = static_cast<int64_t>(seq);

    E2eEncryptParams params;
    params.text = text;
    params.channel = E2eRelayPayloadCodec::ChannelFromThread(channel);
    params.peer_contact_id = local_account;
    params.sender_contact_id = peer_account;
    params.message_id = envelope.message_id;
    params.sender_seq = seq;
    params.session_epoch = 1;
    params.timestamp = envelope.timestamp;
    auto payload = E2eRelayPayloadCodec::EncryptChatPayloadBytes(params, *plaintext, TestMasterPsk());
    if (!payload) {
      throw std::runtime_error("Failed to encrypt system payload");
    }
    envelope.body.e2e.payload_b64 = *payload;

    auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
    if (!sign_bytes) {
      throw std::runtime_error("Failed to build sign bytes");
    }
    auto signature =
        MlDsa::Sign(peer_keys.secret_key, *sign_bytes);
    if (!signature) {
      throw std::runtime_error("Failed to sign envelope");
    }
    envelope.signature = Base64Encode(*signature);
    return envelope;
  }

  std::filesystem::path data_dir;
  SqliteThreadStore store;
  test::TestAccount keys;
  FakeContacts contacts;
  GroupRosterStore roster;
  SqlitePskSessionStore psk_store;
  PeerSigningKeyStore key_store;
  PeerSigningKeyResolver key_resolver;
  GroupInviteGate gate;
  RelayReceivePipeline pipeline;
  MlDsaKeyPair peer_keys;
  Thread dm_thread;
  std::string local_account;
  std::string peer_account;
  std::string local_relay;
  std::string peer_relay;
  ThreadChannel channel = ThreadChannel::E2e;
};

TEST(GroupInviteIngestTest, InboundInviteStoresPendingWithTitleAndActions) {
  PartyHarness invitee("invitee", "account:bob", "account:alice");

  GroupInvitePayload invite;
  invite.group_id = "group:hike";
  invite.group_title = "Weekend hike";
  invite.inviter_identity = "account:alice";
  invite.invitee_identity = "account:bob";
  invite.invite_nonce = "nonce-invite-1";
  invite.roster_epoch = 1;
  invite.expires_at = util::NowUnixMs() + 86400000;
  auto detail = GroupMembershipCodec::EncodeInvite(invite);
  ASSERT_TRUE(detail);

  const RelayEnvelope envelope =
      invitee.MakeSystemEnvelope("group_invite", *detail, "Group invitation: Weekend hike", 1);
  const RelayReceiveOutcome outcome = invitee.pipeline.ProcessEnvelope(envelope, "account:bob");
  EXPECT_TRUE(outcome.persisted) << "decision=" << static_cast<int>(outcome.decision);

  auto pending = invitee.roster.LoadPendingInvite("nonce-invite-1");
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->group_title, "Weekend hike");
  EXPECT_EQ((*pending)->roster_epoch, 1u);
  EXPECT_EQ((*pending)->status, InviteStatus::Pending);

  auto messages = invitee.store.GetMessages(invitee.dm_thread.id);
  ASSERT_TRUE(messages);
  ASSERT_FALSE(messages->empty());
  EXPECT_EQ(messages->back().chat_actions.size(), 3u);
}

TEST(GroupInviteIngestTest, InboundAcceptAddsInviteeToOwnerRoster) {
  PartyHarness owner("owner", "account:alice", "account:bob");

  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(owner.roster.UpsertMetadata(metadata));
  GroupRosterMember owner_member;
  owner_member.member_identity = "account:alice";
  owner_member.role = MemberRole::Owner;
  owner_member.joined_at = util::NowUnixMs();
  ASSERT_TRUE(owner.roster.UpsertMember("group:hike", owner_member));
  auto group_thread = owner.store.FindOrCreateGroupThread("group:hike", "Weekend hike", {});
  ASSERT_TRUE(group_thread);
  ASSERT_TRUE(owner.roster.UpsertGroupTarget("group:hike", group_thread->id, 1, 1));

  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-accept-1";
  pending.group_id = "group:hike";
  pending.group_title = "Weekend hike";
  pending.inviter_identity = "account:alice";
  pending.invitee_identity = "account:bob";
  pending.roster_epoch = 1;
  pending.status = InviteStatus::Pending;
  pending.created_at = util::NowUnixMs();
  pending.outbound = true;  // alice issued it
  ASSERT_TRUE(owner.roster.UpsertPendingInvite(pending));

  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-accept-1", "group:hike");
  ASSERT_TRUE(detail);
  const RelayEnvelope envelope =
      owner.MakeSystemEnvelope("group_invite_accept", *detail, "Accepted group invitation", 1);
  const RelayReceiveOutcome outcome = owner.pipeline.ProcessEnvelope(envelope, "account:alice");
  EXPECT_TRUE(outcome.persisted) << "decision=" << static_cast<int>(outcome.decision);
  ASSERT_TRUE(outcome.publish_member_joined_group_id);
  EXPECT_EQ(*outcome.publish_member_joined_group_id, "group:hike");
  ASSERT_TRUE(outcome.publish_member_joined_member_identity);
  EXPECT_EQ(*outcome.publish_member_joined_member_identity, "account:bob");
  EXPECT_EQ(outcome.publish_member_joined_epoch, 2u);

  auto members = owner.roster.ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 2u);
  bool found_bob = false;
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity == "account:bob") {
      found_bob = true;
      EXPECT_EQ(member.role, MemberRole::Member);
    }
  }
  EXPECT_TRUE(found_bob);

  auto meta = owner.roster.LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->roster_epoch, 2u);

  // Non-member still rejected from group envelopes until accept (bob is now member on owner).
  // Verify skip-self encrypt would include bob.
  std::vector<GroupMemberTarget> targets;
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity == "account:alice") {
      continue;
    }
    GroupMemberTarget target;
    target.member_identity = member.member_identity;
    target.target_key = GroupE2ePayloadCodec::PairTargetKey(member.member_identity);
    targets.push_back(std::move(target));
  }
  EXPECT_EQ(targets.size(), 1u);
}

TEST(GroupInviteIngestTest, InboundAcceptAppliesEvenWhenDmSoftCompromised) {
  // Private e2e: compromised latches; membership bypass still applies accept.
  PartyHarness owner("accept_compromised", "account:alice", "account:bob", ThreadChannel::E2e);

  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(owner.roster.UpsertMetadata(metadata));
  GroupRosterMember owner_member;
  owner_member.member_identity = "account:alice";
  owner_member.role = MemberRole::Owner;
  owner_member.joined_at = util::NowUnixMs();
  ASSERT_TRUE(owner.roster.UpsertMember("group:hike", owner_member));
  auto group_thread = owner.store.FindOrCreateGroupThread("group:hike", "Weekend hike", {});
  ASSERT_TRUE(group_thread);

  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-accept-compromised";
  pending.group_id = "group:hike";
  pending.group_title = "Weekend hike";
  pending.inviter_identity = "account:alice";
  pending.invitee_identity = "account:bob";
  pending.roster_epoch = 1;
  pending.status = InviteStatus::Pending;
  pending.created_at = util::NowUnixMs();
  pending.outbound = true;  // alice issued it
  ASSERT_TRUE(owner.roster.UpsertPendingInvite(pending));

  PeerSyncState compromised = DefaultPeerSyncState();
  compromised.contiguous_peer_seq = 5;
  compromised.loaded_min_seq = 1;
  compromised.loaded_max_seq = 5;
  compromised.phase = PeerSyncPhase::Compromised;
  ASSERT_TRUE(owner.store.SetPeerSyncState(owner.dm_thread.id, 1, compromised));

  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-accept-compromised", "group:hike");
  ASSERT_TRUE(detail);
  const RelayEnvelope envelope =
      owner.MakeSystemEnvelope("group_invite_accept", *detail, "Accepted group invitation", 6);
  const RelayReceiveOutcome outcome = owner.pipeline.ProcessEnvelope(envelope, "account:alice");
  EXPECT_EQ(outcome.decision, IngestDecision::SoftCompromised);
  EXPECT_TRUE(outcome.persisted) << "membership accept must persist despite SoftCompromised";
  ASSERT_TRUE(outcome.publish_member_joined_group_id);
  EXPECT_EQ(*outcome.publish_member_joined_member_identity, "account:bob");

  auto members = owner.roster.ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 2u);
}

TEST(GroupInviteIngestTest, InboundAcceptOnPublicClearsCompromisedLatch) {
  // D046: e2e_public clears compromised and continues normal ingest.
  PartyHarness owner("accept_public_relaxed", "account:alice", "account:bob", ThreadChannel::E2ePublic);

  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(owner.roster.UpsertMetadata(metadata));
  GroupRosterMember owner_member;
  owner_member.member_identity = "account:alice";
  owner_member.role = MemberRole::Owner;
  owner_member.joined_at = util::NowUnixMs();
  ASSERT_TRUE(owner.roster.UpsertMember("group:hike", owner_member));

  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-accept-public";
  pending.group_id = "group:hike";
  pending.group_title = "Weekend hike";
  pending.inviter_identity = "account:alice";
  pending.invitee_identity = "account:bob";
  pending.roster_epoch = 1;
  pending.status = InviteStatus::Pending;
  pending.created_at = util::NowUnixMs();
  pending.outbound = true;  // alice issued it
  ASSERT_TRUE(owner.roster.UpsertPendingInvite(pending));

  PeerSyncState compromised = DefaultPeerSyncState();
  compromised.contiguous_peer_seq = 5;
  compromised.loaded_min_seq = 1;
  compromised.loaded_max_seq = 5;
  compromised.phase = PeerSyncPhase::Compromised;
  ASSERT_TRUE(owner.store.SetPeerSyncState(owner.dm_thread.id, 1, compromised));

  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-accept-public", "group:hike");
  ASSERT_TRUE(detail);
  const RelayEnvelope envelope =
      owner.MakeSystemEnvelope("group_invite_accept", *detail, "Accepted group invitation", 6);
  const RelayReceiveOutcome outcome = owner.pipeline.ProcessEnvelope(envelope, "account:alice");
  EXPECT_EQ(outcome.decision, IngestDecision::AcceptContiguous);
  EXPECT_TRUE(outcome.persisted);
  ASSERT_TRUE(outcome.publish_member_joined_group_id);
  EXPECT_EQ(*outcome.publish_member_joined_member_identity, "account:bob");

  auto sync = owner.store.GetPeerSyncState(owner.dm_thread.id, 1);
  ASSERT_TRUE(sync);
  EXPECT_NE(sync->phase, PeerSyncPhase::Compromised);
  EXPECT_EQ(sync->contiguous_peer_seq, 6u);

  auto members = owner.roster.ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 2u);
}

TEST(GroupInviteIngestTest, InboundAcceptWithoutPendingIsRejected) {
  PartyHarness owner("accept_no_pending", "account:alice", "account:bob");

  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(owner.roster.UpsertMetadata(metadata));
  GroupRosterMember owner_member;
  owner_member.member_identity = "account:alice";
  owner_member.role = MemberRole::Owner;
  owner_member.joined_at = util::NowUnixMs();
  ASSERT_TRUE(owner.roster.UpsertMember("group:hike", owner_member));

  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-missing", "group:hike");
  ASSERT_TRUE(detail);
  const RelayEnvelope envelope =
      owner.MakeSystemEnvelope("group_invite_accept", *detail, "Accepted group invitation", 1);
  const RelayReceiveOutcome outcome = owner.pipeline.ProcessEnvelope(envelope, "account:alice");
  EXPECT_EQ(outcome.decision, IngestDecision::HardReject);
  EXPECT_FALSE(outcome.persisted);
  EXPECT_FALSE(outcome.publish_member_joined_group_id);
  auto members = owner.roster.ListMembers("group:hike");
  ASSERT_TRUE(members);
  EXPECT_EQ(members->size(), 1u);
  ASSERT_TRUE(outcome.receive_failure.has_value());
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::MembershipUpdateFailed);
}

TEST(GroupInviteIngestTest, InboundMemberJoinedAddsPeerToRoster) {
  PartyHarness member("joined_peer", "account:carol", "account:alice");

  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(member.roster.UpsertMetadata(metadata));
  GroupRosterMember alice;
  alice.member_identity = "account:alice";
  alice.role = MemberRole::Owner;
  alice.joined_at = util::NowUnixMs();
  GroupRosterMember carol;
  carol.member_identity = "account:carol";
  carol.role = MemberRole::Member;
  carol.joined_at = util::NowUnixMs();
  ASSERT_TRUE(member.roster.UpsertMember("group:hike", alice));
  ASSERT_TRUE(member.roster.UpsertMember("group:hike", carol));
  auto group_thread = member.store.FindOrCreateGroupThread("group:hike", "Weekend hike", {});
  ASSERT_TRUE(group_thread);
  ASSERT_TRUE(member.roster.UpsertGroupTarget("group:hike", group_thread->id, 1, 1));

  auto detail = GroupMembershipCodec::EncodeMemberJoined("group:hike", "account:bob", MemberRole::Member, 2);
  ASSERT_TRUE(detail);
  const RelayEnvelope envelope =
      member.MakeSystemEnvelope("member_joined", *detail, "Member joined the group", 1);
  const RelayReceiveOutcome outcome = member.pipeline.ProcessEnvelope(envelope, "account:carol");
  EXPECT_TRUE(outcome.persisted) << "decision=" << static_cast<int>(outcome.decision);

  auto members = member.roster.ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 3u);
  auto meta = member.roster.LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->roster_epoch, 2u);
}

TEST(GroupInviteIngestTest, InboundMemberJoinedHandsOverTheOwnersCardsAndStoresNone) {
  PartyHarness member("joined_cards", "account:carol", "account:alice");
  std::vector<std::string> handed;
  std::string bob_route;
  member.pipeline.SetOnRosterMember([&](const GroupMembershipCodec::MemberJoinedEntry& entry) {
    handed.push_back(entry.member_identity);
    if (entry.member_identity == "account:bob") {
      bob_route = entry.relay_user_id;
    }
  });

  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(member.roster.UpsertMetadata(metadata));
  ASSERT_TRUE(member.roster.UpsertMember(
      "group:hike", GroupRosterMember{.member_identity = "account:alice", .role = MemberRole::Owner}));
  ASSERT_TRUE(member.roster.UpsertMember(
      "group:hike", GroupRosterMember{.member_identity = "account:carol", .role = MemberRole::Member}));

  Object card;
  card.set("account_id", "account:bob");
  GroupMembershipCodec::MemberJoinedPayload payload{.group_id = "group:hike",
                                                    .member_identity = "account:carol",
                                                    .roster_epoch = 2};
  payload.members = {{.member_identity = "account:alice", .role = MemberRole::Owner},
                     {.member_identity = "account:bob", .card = card, .relay_user_id = "relay:bob"},
                     {.member_identity = "account:carol", .card = card}};
  auto detail = GroupMembershipCodec::EncodeMemberJoined(payload);
  ASSERT_TRUE(detail);
  const RelayReceiveOutcome outcome = member.pipeline.ProcessEnvelope(
      member.MakeSystemEnvelope("member_joined", *detail, "Member joined the group", 1), "account:carol");
  ASSERT_TRUE(outcome.persisted) << "decision=" << static_cast<int>(outcome.decision);

  EXPECT_EQ(handed, std::vector<std::string>{"account:bob"}) << "each other member the owner says something of";
  EXPECT_EQ(bob_route, "relay:bob");
  ASSERT_TRUE(outcome.persisted_message.has_value());
  auto stored = GroupMembershipCodec::DecodeMemberJoinedFromMessage(*outcome.persisted_message);
  ASSERT_TRUE(stored) << stored.error().message;
  ASSERT_EQ(stored->members.size(), 3u) << "the roster stays";
  for (const auto& entry : stored->members) {
    EXPECT_FALSE(entry.card.has_value()) << entry.member_identity;
  }
  EXPECT_EQ(stored->members[1].relay_user_id, "relay:bob");
}

TEST(GroupInviteIngestTest, InboundOwnerTransferredLeavePreviousUpdatesRoster) {
  PartyHarness member("xfer_member", "account:bob", "account:alice");

  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(member.roster.UpsertMetadata(metadata));
  GroupRosterMember alice;
  alice.member_identity = "account:alice";
  alice.role = MemberRole::Owner;
  alice.joined_at = util::NowUnixMs();
  GroupRosterMember bob;
  bob.member_identity = "account:bob";
  bob.role = MemberRole::Member;
  bob.joined_at = util::NowUnixMs();
  ASSERT_TRUE(member.roster.UpsertMember("group:hike", alice));
  ASSERT_TRUE(member.roster.UpsertMember("group:hike", bob));
  auto group_thread = member.store.FindOrCreateGroupThread("group:hike", "Weekend hike", {});
  ASSERT_TRUE(group_thread);
  ASSERT_TRUE(member.roster.UpsertGroupTarget("group:hike", group_thread->id, 1, 1));

  auto detail = GroupMembershipCodec::EncodeOwnerTransferred("group:hike", "account:bob", 2, true);
  ASSERT_TRUE(detail);
  const RelayEnvelope envelope =
      member.MakeSystemEnvelope("owner_transferred", *detail, "Group ownership transferred", 1);
  const RelayReceiveOutcome outcome = member.pipeline.ProcessEnvelope(envelope, "account:bob");
  EXPECT_TRUE(outcome.persisted) << "decision=" << static_cast<int>(outcome.decision);

  auto meta = member.roster.LoadMetadata("group:hike");
  ASSERT_TRUE(meta && meta->has_value());
  EXPECT_EQ((*meta)->owner_identity, "account:bob");
  EXPECT_EQ((*meta)->roster_epoch, 2u);
  auto members = member.roster.ListMembers("group:hike");
  ASSERT_TRUE(members);
  ASSERT_EQ(members->size(), 1u);
  EXPECT_EQ(members->front().member_identity, "account:bob");
}

TEST(GroupInviteIngestTest, GroupEnvelopeHardRejectsClosedThreadEvenIfStillMember) {
  PartyHarness party("closed_thread", "account:bob", "account:alice");
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(party.roster.UpsertMetadata(metadata));
  GroupRosterMember bob;
  bob.member_identity = "account:bob";
  bob.role = MemberRole::Member;
  bob.joined_at = util::NowUnixMs();
  ASSERT_TRUE(party.roster.UpsertMember("group:hike", bob));
  // No local group thread (user deleted/dismissed) — must not resurrect.
  ASSERT_FALSE(party.store.FindGroupThread("group:hike")->has_value());

  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = util::GenerateUuid();
  envelope.sender_contact_id = "account:alice";
  envelope.sender_relay_id = "relay:alice";
  envelope.route.kind = "group";
  envelope.route.group_id = "group:hike";
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope.body.e2e.member_payloads = std::map<std::string, std::string>{{"account:bob", "deadbeef"}};
  const RelayReceiveOutcome outcome = party.pipeline.ProcessEnvelope(envelope, "account:bob");
  EXPECT_EQ(outcome.decision, IngestDecision::HardReject);
  EXPECT_FALSE(outcome.persisted);
  EXPECT_FALSE(party.store.FindGroupThread("group:hike")->has_value());
}

TEST(GroupInviteIngestTest, GroupEnvelopeHardRejectsNonMember) {
  PartyHarness owner("nonmember", "account:alice", "account:bob");
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(owner.roster.UpsertMetadata(metadata));
  GroupRosterMember owner_member;
  owner_member.member_identity = "account:alice";
  owner_member.role = MemberRole::Owner;
  owner_member.joined_at = util::NowUnixMs();
  ASSERT_TRUE(owner.roster.UpsertMember("group:hike", owner_member));

  // Local is alice (member). Simulate bob receiving while not on bob's roster — use bob harness.
  PartyHarness invitee("nonmember_bob", "account:bob", "account:alice");
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = util::GenerateUuid();
  envelope.sender_contact_id = "account:alice";
  envelope.sender_relay_id = "relay:alice";
  envelope.route.kind = "group";
  envelope.route.group_id = "group:hike";
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope.body.e2e.member_payloads = std::map<std::string, std::string>{{"account:bob", "deadbeef"}};
  const RelayReceiveOutcome outcome = invitee.pipeline.ProcessEnvelope(envelope, "account:bob");
  EXPECT_EQ(outcome.decision, IngestDecision::HardReject);
  EXPECT_FALSE(outcome.persisted);
}

namespace {

RelayEnvelope GroupEnvelopeFrom(const std::string& sender, const std::string& group_id, const std::string& to) {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = util::GenerateUuid();
  envelope.sender_contact_id = sender;
  envelope.sender_relay_id = PartyHarness::RelayForAccount(sender);
  envelope.route.kind = "group";
  envelope.route.group_id = group_id;
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope.body.e2e.member_payloads = std::map<std::string, std::string>{{to, "deadbeef"}};
  return envelope;
}

void SeedHike(PartyHarness& party, const std::vector<std::string>& members) {
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.title = "Weekend hike";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(party.roster.UpsertMetadata(metadata));
  for (const std::string& identity : members) {
    GroupRosterMember member;
    member.member_identity = identity;
    member.role = identity == "account:alice" ? MemberRole::Owner : MemberRole::Member;
    member.joined_at = util::NowUnixMs();
    ASSERT_TRUE(party.roster.UpsertMember("group:hike", member));
  }
  ASSERT_TRUE(party.store.FindOrCreateGroupThread("group:hike", "Weekend hike", {}));
}

}  // namespace

// Group messages need both ends on this device's roster: dropped (silently, before signature and
// decrypt) when the sender is not a member...
TEST(GroupInviteIngestTest, GroupEnvelopeFromASenderOffTheRosterIsDropped) {
  PartyHarness bob("offroster", "account:bob", "account:alice");
  SeedHike(bob, {"account:bob", "account:carol"});
  const RelayReceiveOutcome outcome =
      bob.pipeline.ProcessEnvelope(GroupEnvelopeFrom("account:alice", "group:hike", "account:bob"), "account:bob");
  EXPECT_EQ(outcome.decision, IngestDecision::HardReject);
  EXPECT_FALSE(outcome.persisted);
  EXPECT_FALSE(outcome.receive_failure) << "dropped on the roster, not at signature / decrypt";
}

// ...and when we are no longer a member, though the group's thread is still there (removed by the owner).
TEST(GroupInviteIngestTest, GroupEnvelopeAfterLocalRemovalIsDropped) {
  PartyHarness bob("removed", "account:bob", "account:alice");
  SeedHike(bob, {"account:alice"});
  ASSERT_TRUE(bob.store.FindGroupThread("group:hike")->has_value());
  const RelayReceiveOutcome outcome =
      bob.pipeline.ProcessEnvelope(GroupEnvelopeFrom("account:alice", "group:hike", "account:bob"), "account:bob");
  EXPECT_EQ(outcome.decision, IngestDecision::HardReject);
  EXPECT_FALSE(outcome.persisted);
  EXPECT_FALSE(outcome.receive_failure) << "dropped on the roster, not at signature / decrypt";
}

// An invite is recorded only when its sender is the inviter and we are the invitee, and only from
// the owner of a group we already know.
TEST(GroupInviteIngestTest, AnInviteIsRecordedOnlyFromItsInviterToUs) {
  PartyHarness bob("invite_bind", "account:bob", "account:alice");
  const auto send = [&bob](GroupInvitePayload invite, uint64_t seq) {
    invite.group_title = "Weekend hike";
    invite.roster_epoch = 1;
    invite.expires_at = util::NowUnixMs() + 86400000;
    auto detail = GroupMembershipCodec::EncodeInvite(invite);
    EXPECT_TRUE(detail);
    (void)bob.pipeline.ProcessEnvelope(bob.MakeSystemEnvelope("group_invite", *detail, "Group invitation", seq),
                                       "account:bob");
    auto pending = bob.roster.LoadPendingInvite(invite.invite_nonce);
    return pending && pending->has_value();
  };
  GroupInvitePayload invite;
  invite.group_id = "group:hike";
  invite.inviter_identity = "account:alice";
  invite.invitee_identity = "account:carol";
  invite.invite_nonce = "not-for-us";
  EXPECT_FALSE(send(invite, 1)) << "addressed to someone else";
  invite.inviter_identity = "account:dave";
  invite.invitee_identity = "account:bob";
  invite.invite_nonce = "not-from-sender";
  EXPECT_FALSE(send(invite, 2)) << "names another inviter";

  GroupMetadata known;
  known.group_id = "group:known";
  known.owner_identity = "account:dave";
  known.roster_epoch = 1;
  ASSERT_TRUE(bob.roster.UpsertMetadata(known));
  invite.group_id = "group:known";
  invite.inviter_identity = "account:alice";
  invite.invite_nonce = "not-the-owner";
  EXPECT_FALSE(send(invite, 3)) << "a known group the sender does not own";

  invite.group_id = "group:hike";
  invite.invite_nonce = "fine";
  EXPECT_TRUE(send(invite, 4));
}

} // namespace

namespace pbr {
namespace {

// projects/privacy T3: a Blocked sender's direct messages (chat and call control alike) are dropped
// on receipt — before the thread, the inbox or a ring sees them.
TEST(BlockedSenderIngestTest, DirectEnvelopesFromABlockedSenderAreDropped) {
  PartyHarness invitee("blocked-sender", "account:bob", "account:alice");
  invitee.pipeline.SetContactTrust(&invitee.contacts);

  GroupInvitePayload invite;
  invite.group_id = "group:hike";
  invite.group_title = "Weekend hike";
  invite.inviter_identity = "account:alice";
  invite.invitee_identity = "account:bob";
  invite.invite_nonce = "nonce-before-block";
  invite.roster_epoch = 1;
  invite.expires_at = util::NowUnixMs() + 86400000;
  auto detail = GroupMembershipCodec::EncodeInvite(invite);
  ASSERT_TRUE(detail);
  RelayReceiveOutcome outcome = invitee.pipeline.ProcessEnvelope(
      invitee.MakeSystemEnvelope("group_invite", *detail, "Group invitation", 1), "account:bob");
  ASSERT_TRUE(outcome.persisted) << "a Friendly contact's message lands";

  ASSERT_TRUE(invitee.contacts.BlockAccountIfPresent("account:alice"));
  invite.invite_nonce = "nonce-after-block";
  detail = GroupMembershipCodec::EncodeInvite(invite);
  ASSERT_TRUE(detail);
  outcome = invitee.pipeline.ProcessEnvelope(
      invitee.MakeSystemEnvelope("group_invite", *detail, "Group invitation", 2), "account:bob");
  EXPECT_FALSE(outcome.persisted);
  EXPECT_EQ(outcome.decision, IngestDecision::SilentDiscard);
  auto pending = invitee.roster.LoadPendingInvite("nonce-after-block");
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value()) << "the Blocked sender's control applied";
}

} // namespace
} // namespace pbr
