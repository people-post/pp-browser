#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/E2eIngestClassifier.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/GroupE2ePayloadCodec.h"
#include "base/messaging/GroupMembershipApply.h"
#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/PeerSigningKeyStore.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/people/ContactTypes.h"
#include "base/people/ContactsStore.h"
#include "base/crypto/MlDsa.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/GroupInviteGate.h"
#include "feature/messaging/RelayReceivePipeline.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>

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
        store(data_dir.string()), identity(data_dir.string(), "test"), roster(store.ProfileDbPath()),
        contacts(data_dir.string()), psk_store(store.ProfileDbPath(), "test"), key_resolver(key_store),
        gate(contacts, roster), pipeline(store, key_resolver, psk_store, identity, roster, &gate),
        local_account(local_account), peer_account(peer_account),
        local_relay(RelayForAccount(local_account)), peer_relay(RelayForAccount(peer_account)), channel(channel) {
    std::filesystem::remove_all(data_dir);
    std::filesystem::create_directories(data_dir);
    if (!identity.SetDek(TestDek()) || !psk_store.SetDek(TestDek())) {
      throw std::runtime_error("Failed to set test DEK");
    }
    auto loaded = identity.LoadOrCreate();
    if (!loaded) {
      throw std::runtime_error("Failed to load identity");
    }
    LocalIdentity id = *loaded;
    id.account_id = local_account;
    id.relay_user_id = local_relay;
    if (!identity.Update(id)) {
      throw std::runtime_error("Failed to update identity");
    }

    auto generated = MlDsa::GenerateKeyPair();
    if (!generated) {
      throw std::runtime_error("Failed to generate peer keys");
    }
    peer_keys = *generated;
    PeerSigningKeyRecord record;
    record.signing_public_key_b64 = Base64Encode(peer_keys.public_key);
    record.source = "test";
    key_store.Put("account", peer_account, record);

    Contact contact;
    contact.id = "contact-peer";
    contact.local.display_name = peer_account;
    contact.local.trust = TrustLevel::Friendly;
    contact.remote.ids = {{ContactIdKind::Account, peer_account, true},
                          {ContactIdKind::RelayUser, peer_relay, false}};
    SyncContactMirrors(contact);
    if (!contacts.Upsert(contact)) {
      throw std::runtime_error("Failed to upsert contact");
    }
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
    system.payload_json = nlohmann::json({{"control_type", control_type}, {"detail", detail}}).dump();
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
  IdentityStore identity;
  GroupRosterStore roster;
  ContactsStore contacts;
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
  ASSERT_TRUE(outcome.receive_failure_notice.has_value());
  EXPECT_NE(outcome.receive_failure_notice->find("membership"), std::string::npos);
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

} // namespace
