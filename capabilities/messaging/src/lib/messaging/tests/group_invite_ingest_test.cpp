#include "common/chat/RelayStreamKey.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/E2eIngestClassifier.h"
#include "lib/delivery/E2eRelayPayloadCodec.h"
#include "lib/delivery/EnvelopeSigner.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/people/PeerSigningKeyStore.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "common/directory/IContactTrustAccess.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/messaging/GroupInviteGate.h"
#include "lib/messaging/GroupLinkJoin.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/tests/GroupChangeTestKit.h"
#include "lib/messaging/tests/TestAccount.h"
#include "lib/messaging/tests/TestRosterSigner.h"
#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/GroupRosterSyncCodec.h"
#include "lib/messaging/TranscriptAssembly.h"

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

// Temp-dir cleanup (Windows CI): do not call remove_all while InMemoryDeliveryStore /
// InMemoryGroupRosterStore / InMemoryPeerSessions still hold open DB handles. Member
// destructors close SQLite first; a harness destructor that remove_all's first
// throws on Windows ("being used by another process") and aborts the test process.
// Use a unique path and skip destructor cleanup (same as SyncTestHarness).
struct PartyHarness {
  explicit PartyHarness(const std::string& suffix, const std::string& local_account, const std::string& peer_account,
                        const ThreadChannel channel = ThreadChannel::E2e)
      : data_dir(std::filesystem::temp_directory_path() /
                 ("pp_invite_ingest_" + suffix + "_" + util::GenerateUuid())),
        store(), keys(test::TestAccount::Make()), roster(store),
        psk_store(store), peer(test::TestAccount::Make()), gate(contacts.Standing(), roster),
        pipeline(store,
                 [this, peer_account](const std::string& account) -> std::optional<AccountCard> {
                   // The peer's device as its kept card lists it (filed under the fixture's readable id).
                   return account == peer_account ? std::optional<AccountCard>(peer.DeviceCard()) : std::nullopt;
                 },
                 psk_store,
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

    PeerSessionRecord psk;
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

    test::SignEnvelopeAs(peer, envelope);
    return envelope;
  }

  std::filesystem::path data_dir;
  InMemoryDeliveryStore store;
  test::TestAccount keys;
  FakeContacts contacts;
  InMemoryGroupRosterStore roster;
  InMemoryPeerSessions psk_store;
  /** The peer: its device signs what it sends. */
  test::TestAccount peer;
  GroupInviteGate gate;
  RelayReceivePipeline pipeline;
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

// G022: someone met in a call (not a contact) invites us from it: kept, and shown in that call, only while we are
// in the call with them; it stays the ordinary invite (Accept / Decline / Block on the card).
TEST(GroupInviteIngestTest, AStrangersInviteFromOurLiveCallIsKeptAndShownInTheCall) {
  PartyHarness invitee("call_invitee", "account:bob", "account:alice");
  invitee.contacts.known.clear(); // Alice was brought into the call by someone else
  std::string live_call = "call:live";
  invitee.gate.SetInLiveCall([&live_call](const std::string& call_id, const std::string& inviter) {
    return call_id == live_call && inviter == "account:alice";
  });

  const auto send_invite = [&](const std::string& nonce, const std::string& call_id, const uint64_t seq) {
    GroupInvitePayload invite;
    invite.group_id = "group:call-" + nonce;
    invite.group_title = "Alice, Bob, Carol";
    invite.inviter_identity = "account:alice";
    invite.invitee_identity = "account:bob";
    invite.invite_nonce = nonce;
    invite.roster_epoch = 1;
    invite.expires_at = util::NowUnixMs() + 86400000;
    invite.context = kGroupInviteContextCall;
    invite.call_id = call_id;
    auto detail = GroupMembershipCodec::EncodeInvite(invite);
    EXPECT_TRUE(detail);
    return invitee.pipeline.ProcessEnvelope(
        invitee.MakeSystemEnvelope("group_invite", *detail, "Group invitation: Alice, Bob, Carol", seq), "account:bob");
  };

  (void)send_invite("nonce-in-call", "call:live", 1);
  auto pending = invitee.roster.LoadPendingInvite("nonce-in-call");
  ASSERT_TRUE(pending && pending->has_value()) << "met in the call: the invite reaches us";
  EXPECT_EQ((*pending)->status, InviteStatus::Pending) << "nobody is added without Join";
  const auto in_call = invitee.gate.InvitesInCall("call:live", util::NowUnixMs());
  ASSERT_EQ(in_call.size(), 1u);
  EXPECT_EQ(in_call[0].invite_nonce, "nonce-in-call");
  EXPECT_EQ(in_call[0].inviter_identity, "account:alice");

  (void)send_invite("nonce-other-call", "call:other", 2);
  auto other = invitee.roster.LoadPendingInvite("nonce-other-call");
  EXPECT_FALSE(other && other->has_value()) << "a call we are not in with them is no reason";

  live_call.clear(); // the call is over
  (void)send_invite("nonce-after-call", "call:live", 3);
  auto after = invitee.roster.LoadPendingInvite("nonce-after-call");
  EXPECT_FALSE(after && after->has_value()) << "only while the call goes on";
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

// G014: an accept that comes when the group is full adds no one; the owner is asked to tell the invitee.
TEST(GroupInviteIngestTest, InboundAcceptWhenFullAsksTheOwnerToRefuse) {
  PartyHarness owner("owner", "account:alice", "account:bob");
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(owner.roster.UpsertMetadata(metadata));
  for (size_t i = 0; i < kMaxGroupMembers; ++i) {
    GroupRosterMember member;
    member.member_identity = i == 0 ? "account:alice" : "account:m" + std::to_string(i);
    member.role = i == 0 ? MemberRole::Owner : MemberRole::Member;
    ASSERT_TRUE(owner.roster.UpsertMember("group:hike", member));
  }
  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-full";
  pending.group_id = "group:hike";
  pending.inviter_identity = "account:alice";
  pending.invitee_identity = "account:bob";
  pending.status = InviteStatus::Pending;
  pending.created_at = util::NowUnixMs();
  pending.outbound = true;
  ASSERT_TRUE(owner.roster.UpsertPendingInvite(pending));

  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-full", "group:hike");
  ASSERT_TRUE(detail);
  const RelayReceiveOutcome outcome = owner.pipeline.ProcessEnvelope(
      owner.MakeSystemEnvelope("group_invite_accept", *detail, "Accepted group invitation", 1), "account:alice");
  EXPECT_FALSE(outcome.publish_member_joined_group_id);
  ASSERT_TRUE(outcome.refuse_invite_group_id);
  EXPECT_EQ(*outcome.refuse_invite_group_id, "group:hike");
  EXPECT_EQ(outcome.refuse_invite_nonce, "nonce-full");
  EXPECT_EQ(outcome.refuse_invite_member, "account:bob");
  EXPECT_FALSE(owner.roster.IsMember("group:hike", "account:bob").value());
}

// G014 invitee side: the owner's refusal drops the group made on accept — read-only, with a line saying why.
TEST(GroupInviteIngestTest, AnInviteRefusedByItsOwnerDropsTheGroup) {
  PartyHarness invitee("invitee", "account:bob", "account:alice");
  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-full";
  pending.group_id = "group:hike";
  pending.inviter_identity = "account:alice";
  pending.invitee_identity = "account:bob";
  pending.status = InviteStatus::Accepted;
  pending.created_at = util::NowUnixMs();
  pending.outbound = false;
  ASSERT_TRUE(invitee.roster.UpsertPendingInvite(pending));
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(invitee.roster.UpsertMetadata(metadata));
  GroupRosterMember me;
  me.member_identity = "account:bob";
  ASSERT_TRUE(invitee.roster.UpsertMember("group:hike", me));
  auto thread = invitee.store.FindOrCreateGroupThread("group:hike", "Weekend hike", {});
  ASSERT_TRUE(thread);

  auto detail = GroupMembershipCodec::EncodeInviteRefused("nonce-full", "group:hike", kInviteRefusedGroupFull);
  ASSERT_TRUE(detail);
  (void)invitee.pipeline.ProcessEnvelope(
      invitee.MakeSystemEnvelope("group_invite_refused", *detail, "Group invitation refused", 1), "account:bob");
  EXPECT_FALSE(invitee.roster.IsMember("group:hike", "account:bob").value());
  auto after = invitee.roster.LoadMetadata("group:hike");
  ASSERT_TRUE(after && *after);
  EXPECT_EQ((*after)->sync_state, GroupSyncState::Removed) << "read-only: it never joined";
  bool said_why = false;
  auto rows = invitee.store.GetMessages(thread->id);
  ASSERT_TRUE(rows);
  for (const ThreadMessage& row : *rows) {
    said_why = said_why || row.payload_json.find("group_invite_refused") != std::string::npos;
  }
  EXPECT_TRUE(said_why);
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
  // The state of what Bob's device numbered in its account copies (M021: per sending device on a public chat).
  ASSERT_TRUE(
      owner.store.SetPeerSyncState(owner.dm_thread.id, 1, compromised, owner.peer.device.peer_id + "@account"));

  auto detail = GroupMembershipCodec::EncodeInviteResponse("nonce-accept-public", "group:hike");
  ASSERT_TRUE(detail);
  const RelayEnvelope envelope =
      owner.MakeSystemEnvelope("group_invite_accept", *detail, "Accepted group invitation", 6);
  const RelayReceiveOutcome outcome = owner.pipeline.ProcessEnvelope(envelope, "account:alice");
  EXPECT_EQ(outcome.decision, IngestDecision::AcceptContiguous);
  EXPECT_TRUE(outcome.persisted);
  ASSERT_TRUE(outcome.publish_member_joined_group_id);
  EXPECT_EQ(*outcome.publish_member_joined_member_identity, "account:bob");

  auto sync = owner.store.GetPeerSyncState(owner.dm_thread.id, 1, owner.peer.device.peer_id + "@account");
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

// G019 / G018 item 6: the newcomer joins from a member's bundle — the owner's statement, the member's signed add, the
// members' cards — and checks each itself: nothing it carries is taken on the inviter's word.
class JoinBundleIngestTest : public ::testing::Test {
protected:
  test::Person& alice = test::Named("alice");
  test::Person& bob = test::Named("bob");
  test::Person& carol = test::Named("carol");
  test::Person& erin = test::Named("erin");
  test::Person& frank = test::Named("frank");
  PartyHarness erin_{"join_bundle", erin.Id(), bob.Id()};
  std::vector<std::string> handed_;

  void SetUp() override {
    erin_.pipeline.SetOnRosterMember(
        [this](const GroupMembershipCodec::MemberJoinedEntry& entry) { handed_.push_back(entry.member_identity); });
    // What accepting Bob's invite left on Erin's device (GroupMembershipWorkflow::AcceptInvite).
    PendingGroupInvite pending;
    pending.invite_nonce = "n1";
    pending.group_id = test::kGroupId;
    pending.inviter_identity = bob.Id();
    pending.invitee_identity = erin.Id();
    pending.owner_identity = alice.Id();
    pending.status = InviteStatus::Accepted;
    pending.created_at = 1;
    ASSERT_TRUE(erin_.roster.UpsertPendingInvite(pending));
    GroupMetadata metadata;
    metadata.group_id = test::kGroupId;
    metadata.owner_identity = alice.Id();
    metadata.roster_epoch = 5;
    ASSERT_TRUE(erin_.roster.UpsertMetadata(metadata));
    for (const auto& [identity, role] : {std::pair<std::string, MemberRole>{alice.Id(), MemberRole::Owner},
                                         {bob.Id(), MemberRole::Member},
                                         {erin.Id(), MemberRole::Member}}) {
      ASSERT_TRUE(
          erin_.roster.UpsertMember(test::kGroupId, GroupRosterMember{.member_identity = identity, .role = role}));
    }
  }

  GroupRosterStatement OwnersStatement() {
    GroupRosterStatement draft;
    draft.roster_epoch = 5;
    draft.members = {test::Row(alice.Id(), MemberRole::Owner), test::Row(bob.Id()), test::Row(carol.Id())};
    return test::SignedStatement(alice, draft);
  }

  GroupChange BobsAdd() {
    GroupChange add = bob.Add(erin.Id(), "n1");
    add.signer_card = bob.Card();
    return add;
  }

  RelayReceiveOutcome Bundle(const GroupRosterStatement& statement, const GroupChange& add,
                             const std::string& nonce = "n1") {
    GroupMembershipCodec::MemberJoinedPayload joined{.group_id = test::kGroupId, .member_identity = erin.Id()};
    joined.added_by = bob.Id();
    joined.invite_nonce = nonce;
    Object card;
    card.set("account_id", "a card");
    joined.members = {{.member_identity = alice.Id(), .role = MemberRole::Owner, .card = card},
                      {.member_identity = bob.Id()},
                      {.member_identity = carol.Id(), .card = card, .relay_user_id = "relay:carol"},
                      {.member_identity = frank.Id(), .card = card}};
    std::string detail = GroupMembershipCodec::EncodeMemberJoined(joined).value();
    detail = group_roster_sync_codec::WithStatement(detail, statement).value();
    detail = group_roster_sync_codec::WithChanges(detail, {.changes = {add}}).value();
    return erin_.pipeline.ProcessEnvelope(
        erin_.MakeSystemEnvelope("member_joined", detail, "Member joined the group", ++seq_), erin.Id());
  }

  std::vector<std::string> Members() {
    std::vector<std::string> out;
    const std::vector<GroupRosterMember> members = erin_.roster.ListMembers(test::kGroupId).value();
    for (const GroupRosterMember& m : members) {
      out.push_back(m.member_identity);
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  uint64_t seq_ = 0;
};

TEST_F(JoinBundleIngestTest, TheNewcomerJoinsFromAMembersBundle) {
  const RelayReceiveOutcome outcome = Bundle(OwnersStatement(), BobsAdd());
  ASSERT_TRUE(outcome.persisted) << "decision=" << static_cast<int>(outcome.decision);
  std::vector<std::string> expected{alice.Id(), bob.Id(), carol.Id(), erin.Id()};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(Members(), expected) << "the owner's statement plus Bob's add";
  EXPECT_EQ(erin_.roster.FindMember(test::kGroupId, erin.Id()).value()->added_by, bob.Id());
  std::sort(handed_.begin(), handed_.end());
  std::vector<std::string> keyed{alice.Id(), carol.Id()};
  std::sort(keyed.begin(), keyed.end());
  EXPECT_EQ(handed_, keyed) << "cards for those the roster lists; Frank's rides along but is not in it";
  EXPECT_EQ(erin_.roster.ListGroupChanges(test::kGroupId).value().size(), 1u) << "the add, held until folded";
  ASSERT_TRUE(outcome.persisted_message);
  auto stored = GroupMembershipCodec::DecodeMemberJoinedFromMessage(*outcome.persisted_message);
  ASSERT_TRUE(stored);
  for (const auto& entry : stored->members) {
    EXPECT_FALSE(entry.card.has_value()) << "the stored copy keeps no card";
  }
}

TEST_F(JoinBundleIngestTest, AForgedAddOrStatementBringsNoOneIn) {
  GroupChange forged = BobsAdd();
  forged.invite_nonce = "n2"; // its signature no longer holds
  const RelayReceiveOutcome tampered = Bundle(OwnersStatement(), forged);
  EXPECT_TRUE(tampered.receive_failure) << "the roster it proves does not have us";
  EXPECT_TRUE(handed_.empty()) << "no card kept on the inviter's word";
  EXPECT_TRUE(erin_.roster.ListGroupChanges(test::kGroupId).value().empty());
  EXPECT_TRUE(erin_.roster.IsMember(test::kGroupId, erin.Id()).value())
      << "nothing of a bundle that does not prove the join is kept: the group stays as accepting left it";

  GroupRosterStatement made_up;
  made_up.roster_epoch = 6;
  made_up.members = {test::Row(bob.Id(), MemberRole::Owner), test::Row(alice.Id()), test::Row(erin.Id())};
  const RelayReceiveOutcome by_bob = Bundle(test::SignedStatement(bob, made_up), BobsAdd());
  EXPECT_TRUE(by_bob.receive_failure) << "a statement that is not the owner's (the invite named Alice)";
  EXPECT_TRUE(handed_.empty());

  const RelayReceiveOutcome real = Bundle(OwnersStatement(), BobsAdd());
  EXPECT_FALSE(real.receive_failure) << "the real bundle still lets them in";
  EXPECT_TRUE(erin_.roster.IsMember(test::kGroupId, carol.Id()).value());
}

TEST_F(JoinBundleIngestTest, ABundleForAnInviteWeDidNotTakeFromItsSenderIsRefused) {
  const RelayReceiveOutcome outcome = Bundle(OwnersStatement(), BobsAdd(), "someone-elses");
  EXPECT_TRUE(outcome.receive_failure);
  EXPECT_TRUE(handed_.empty());
  EXPECT_TRUE(erin_.roster.ListGroupChanges(test::kGroupId).value().empty());
}

// G021: a stranger joined by Carol's link — the bundle is taken from the answerer of a join it confirmed (the record
// GroupMembershipWorkflow::JoinByLink keeps, group_link_join::JoinRecord), and checked like any other.
class LinkJoinBundleIngestTest : public ::testing::Test {
protected:
  test::Person& alice = test::Named("alice");
  test::Person& bob = test::Named("bob");
  test::Person& carol = test::Named("carol");
  test::Person& erin = test::Named("erin");
  // The bundle comes from Carol's account.
  PartyHarness erin_{"link_bundle", erin.Id(), carol.Id()};

  /** What confirming the preview of a join answered by `answerer` left on Erin's device. */
  void Confirmed(const std::string& answerer) {
    ASSERT_TRUE(erin_.roster.UpsertPendingInvite(
        group_link_join::JoinRecord("req-1", test::kGroupId, "Hikers", alice.Id(), answerer, erin.Id(), 1)));
    GroupMetadata metadata;
    metadata.group_id = test::kGroupId;
    metadata.owner_identity = alice.Id();
    metadata.roster_epoch = 5;
    ASSERT_TRUE(erin_.roster.UpsertMetadata(metadata));
    for (const std::string& identity : {alice.Id(), answerer, erin.Id()}) {
      ASSERT_TRUE(erin_.roster.UpsertMember(test::kGroupId, GroupRosterMember{.member_identity = identity}));
    }
  }

  RelayReceiveOutcome Bundle(const std::string& nonce = "req-1") {
    GroupRosterStatement draft;
    draft.roster_epoch = 5;
    draft.members = {test::Row(alice.Id(), MemberRole::Owner), test::Row(bob.Id()), test::Row(carol.Id())};
    GroupChange add = carol.AddByLink(erin.Id(), "link-1");
    add.signer_card = carol.Card();
    GroupMembershipCodec::MemberJoinedPayload joined{.group_id = test::kGroupId, .member_identity = erin.Id()};
    joined.added_by = carol.Id();
    joined.invite_nonce = nonce;
    joined.members = {{.member_identity = alice.Id(), .role = MemberRole::Owner},
                      {.member_identity = bob.Id()},
                      {.member_identity = carol.Id()}};
    std::string detail = GroupMembershipCodec::EncodeMemberJoined(joined).value();
    detail = group_roster_sync_codec::WithStatement(detail, test::SignedStatement(alice, draft)).value();
    detail = group_roster_sync_codec::WithChanges(detail, {.changes = {add}}).value();
    return erin_.pipeline.ProcessEnvelope(erin_.MakeSystemEnvelope("member_joined", detail, "Member joined", 1),
                                          erin.Id());
  }
};

TEST_F(LinkJoinBundleIngestTest, TheBundleOfAJoinWeConfirmedComesFromItsAnswerer) {
  Confirmed(carol.Id());
  const RelayReceiveOutcome outcome = Bundle();
  ASSERT_FALSE(outcome.receive_failure) << outcome.receive_failure_detail;
  EXPECT_TRUE(erin_.roster.IsMember(test::kGroupId, bob.Id()).value()) << "the full roster, as the statement says";
  EXPECT_EQ(erin_.roster.FindMember(test::kGroupId, erin.Id()).value()->added_by, carol.Id());
}

TEST_F(LinkJoinBundleIngestTest, ABundleFromAnyoneButTheAnswererIsRefused) {
  Confirmed(bob.Id()); // Bob's device answered our join; Carol sends a bundle
  const RelayReceiveOutcome from_carol = Bundle();
  EXPECT_TRUE(from_carol.receive_failure);
  EXPECT_TRUE(erin_.roster.ListGroupChanges(test::kGroupId).value().empty());
}

TEST_F(LinkJoinBundleIngestTest, ABundleForAJoinWeDidNotConfirmIsRefused) {
  Confirmed(carol.Id());
  const RelayReceiveOutcome outcome = Bundle("another-request");
  EXPECT_TRUE(outcome.receive_failure);
  EXPECT_TRUE(erin_.roster.ListGroupChanges(test::kGroupId).value().empty());
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
  envelope.stream_key = BuildGroupRelayStreamKey("group:hike", "account:alice", "account:bob", "");
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope = WithSingleRelayCopy(std::move(envelope), RelayCopy{.session_epoch = 1, .sender_seq = 1, .payload_b64 = "deadbeef"});
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
  envelope.stream_key = BuildGroupRelayStreamKey("group:hike", "account:alice", "account:bob", "");
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope = WithSingleRelayCopy(std::move(envelope), RelayCopy{.session_epoch = 1, .sender_seq = 1, .payload_b64 = "deadbeef"});
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
  envelope.stream_key = BuildGroupRelayStreamKey(group_id, sender, to, "");
  envelope.sender_seq = 1;
  envelope.session_epoch = 1;
  envelope = WithSingleRelayCopy(std::move(envelope), RelayCopy{.session_epoch = 1, .sender_seq = 1, .payload_b64 = "deadbeef"});
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

// An invite is recorded only when its sender is the inviter and we are the invitee, and never to a group we are in or
// one that ended; a member's invite counts (G019).
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
  invite.invite_nonce = "from-a-member";
  EXPECT_TRUE(send(invite, 3)) << "G019: a member invites too; the owner is checked when the join bundle comes";
  EXPECT_EQ(bob.roster.LoadPendingInvite("from-a-member").value()->Owner(), "account:alice")
      << "an invite that names no owner is the inviter's own";
  ASSERT_TRUE(bob.roster.UpsertMember("group:known", GroupRosterMember{.member_identity = "account:bob"}));
  invite.invite_nonce = "already-in";
  EXPECT_FALSE(send(invite, 5)) << "not to a group we are in";

  invite.group_id = "group:hike";
  invite.invite_nonce = "fine";
  EXPECT_TRUE(send(invite, 4));
}


// G016: the owner dissolves a group for everyone. Bob is a member; the owner signs with its own account key.
class GroupDissolvedIngestTest : public ::testing::Test {
protected:
  static constexpr const char* kGroup = "group:hike";

  GroupDissolvedIngestTest() : bob_("dissolved_" + util::GenerateUuid(), "account:bob", alice_.Id()) {}

  void SetUp() override {
    GroupMetadata metadata;
    metadata.group_id = kGroup;
    metadata.owner_identity = alice_.Id();
    metadata.title = "Weekend hike";
    metadata.roster_epoch = 3;
    ASSERT_TRUE(bob_.roster.UpsertMetadata(metadata));
    for (const auto& [identity, role] : std::vector<std::pair<std::string, MemberRole>>{
             {alice_.Id(), MemberRole::Owner}, {"account:bob", MemberRole::Member}, {"account:carol", MemberRole::Member}}) {
      GroupRosterMember member;
      member.member_identity = identity;
      member.role = role;
      member.joined_at = 1000;
      ASSERT_TRUE(bob_.roster.UpsertMember(kGroup, member));
    }
    auto thread = bob_.store.FindOrCreateGroupThread(kGroup, "Weekend hike", {});
    ASSERT_TRUE(thread);
    group_thread_ = *thread;
    ASSERT_TRUE(bob_.roster.UpsertGroupTarget(kGroup, group_thread_.id, 1, 1));
    ThreadMessage said;
    said.id = "m1";
    said.thread_id = group_thread_.id;
    said.sender_contact_id = "account:carol";
    said.content_type = ChatContentType::Text;
    said.text = "see you at eight";
    said.timestamp = 1;
    ASSERT_TRUE(bob_.store.AppendMessage(said));
  }

  /** The owner's roster statement (epoch 4: one past bob's), signed as a dissolve unless told otherwise. */
  GroupRosterStatement Statement(test::TestSigner& owner, const bool dissolved = true, const uint64_t epoch = 4) {
    std::vector<GroupRosterStatementMember> rows{{owner.Id(), MemberRole::Owner, 1000},
                                                 {"account:bob", MemberRole::Member, 1000},
                                                 {"account:carol", MemberRole::Member, 1000}};
    return group_roster_statement::Sign(owner, owner.Key(), kGroup, epoch, rows, {}, dissolved).value();
  }

  RelayReceiveOutcome Receive(const GroupRosterStatement& statement) {
    return bob_.pipeline.ProcessEnvelope(
        bob_.MakeSystemEnvelope("group_dissolved", group_roster_sync_codec::EncodeStatement(statement),
                                "The group owner dissolved this group", ++seq_),
        "account:bob");
  }

  GroupMetadata Metadata() { return bob_.roster.LoadMetadata(kGroup).value().value(); }
  std::vector<ThreadMessage> GroupMessages() { return bob_.store.GetMessages(group_thread_.id).value(); }
  static size_t DissolvedLines(const std::vector<ThreadMessage>& messages) {
    size_t n = 0;
    for (const ThreadMessage& m : messages) {
      const auto type = GroupMembershipCodec::ControlTypeFromMessage(m);
      n += type && *type == GroupMembershipControlType::GroupDissolved ? 1 : 0;
    }
    return n;
  }

  test::TestSigner alice_;
  PartyHarness bob_;
  Thread group_thread_;
  uint64_t seq_ = 0;
};

TEST_F(GroupDissolvedIngestTest, TheOwnersDissolveEmptiesTheRosterAndKeepsTheChat) {
  const RelayReceiveOutcome outcome = Receive(Statement(alice_));
  EXPECT_TRUE(outcome.persisted) << "decision=" << static_cast<int>(outcome.decision);
  EXPECT_FALSE(outcome.receive_failure);

  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Dissolved);
  EXPECT_EQ(Metadata().roster_epoch, 4u);
  EXPECT_TRUE(bob_.roster.ListMembers(kGroup).value().empty());
  EXPECT_FALSE(group_roster_sync::MaySend(Metadata())) << "no sends in a dissolved group";
  EXPECT_FALSE(bob_.roster.FindThreadIdForGroup(kGroup).value().has_value()) << "no send target";

  // The chat and its messages stay, with one line.
  ASSERT_TRUE(bob_.store.FindGroupThread(kGroup)->has_value());
  const auto messages = GroupMessages();
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages.front().text, "see you at eight");
  EXPECT_EQ(DissolvedLines(messages), 1u);
  EXPECT_EQ(messages.back().sender_contact_id, alice_.Id()) << "the owner is the line's sender";
}

TEST_F(GroupDissolvedIngestTest, TheGroupGetsTheOneNoticeAndTheDirectChatNone) {
  const RelayReceiveOutcome outcome = Receive(Statement(alice_));
  EXPECT_TRUE(outcome.suppress_inbox_chrome) << "the DM that carried it bumps no unread count and raises no notice";
  ASSERT_TRUE(outcome.dissolved_group_thread_id);
  EXPECT_EQ(*outcome.dissolved_group_thread_id, group_thread_.id);

  // In the group the line shows; in the direct chat that carried it nothing does.
  const auto group_rows = AssembleTranscript(GroupMessages(), nullptr, ThreadKind::Group);
  size_t group_lines = 0;
  for (const TranscriptRow& row : group_rows) {
    const auto type = GroupMembershipCodec::ControlTypeFromMessage(row.message);
    group_lines += type && *type == GroupMembershipControlType::GroupDissolved ? 1 : 0;
  }
  EXPECT_EQ(group_lines, 1u);
  const auto dm_messages = bob_.store.GetMessages(bob_.dm_thread.id).value();
  EXPECT_EQ(DissolvedLines(dm_messages), 1u) << "the wire copy is stored in the DM";
  for (const TranscriptRow& row : AssembleTranscript(dm_messages, nullptr, ThreadKind::Direct)) {
    const auto type = GroupMembershipCodec::ControlTypeFromMessage(row.message);
    EXPECT_FALSE(type && *type == GroupMembershipControlType::GroupDissolved) << "never a line in a direct chat";
  }
}

TEST_F(GroupDissolvedIngestTest, ADissolveFromAnyoneButTheOwnerIsRejected) {
  test::TestSigner carol;
  // A member signs a dissolve of its own: it is not the owner we know...
  EXPECT_TRUE(Receive(Statement(carol)).receive_failure) << "signed by someone who is not the owner we know";
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);
  EXPECT_EQ(bob_.roster.ListMembers(kGroup).value().size(), 3u);

  // ...the owner's signed roster is not a dissolve...
  EXPECT_TRUE(Receive(Statement(alice_, /*dissolved=*/false)).receive_failure);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);

  // ...nor can one be made of it: the mark is part of what the owner signs.
  GroupRosterStatement forged = Statement(alice_, /*dissolved=*/false);
  forged.dissolved = true;
  EXPECT_TRUE(Receive(forged).receive_failure);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);
  EXPECT_EQ(DissolvedLines(GroupMessages()), 0u);
}

TEST_F(GroupDissolvedIngestTest, ADissolveSentByAMemberIsRejected) {
  // Carol, a member, sends it: the owner's genuine dissolve in her name, and one she signed herself.
  test::TestSigner carol;
  PartyHarness from_carol("carol_" + util::GenerateUuid(), "account:bob", carol.Id());
  ASSERT_TRUE(from_carol.roster.UpsertMetadata(Metadata()));
  GroupRosterMember bob;
  bob.member_identity = "account:bob";
  ASSERT_TRUE(from_carol.roster.UpsertMember(kGroup, bob));
  uint64_t seq = 0;
  for (const GroupRosterStatement& statement : {Statement(alice_), Statement(carol)}) {
    const RelayReceiveOutcome outcome = from_carol.pipeline.ProcessEnvelope(
        from_carol.MakeSystemEnvelope("group_dissolved", group_roster_sync_codec::EncodeStatement(statement),
                                      "The group owner dissolved this group", ++seq),
        "account:bob");
    EXPECT_TRUE(outcome.receive_failure);
    EXPECT_EQ(from_carol.roster.LoadMetadata(kGroup).value().value().sync_state, GroupSyncState::Active);
  }
}

TEST_F(GroupDissolvedIngestTest, ARepeatedDissolveDoesNothing) {
  ASSERT_TRUE(Receive(Statement(alice_)).dissolved_group_thread_id);
  const RelayReceiveOutcome again = Receive(Statement(alice_, true, 5));
  EXPECT_FALSE(again.dissolved_group_thread_id) << "no second notice";
  EXPECT_FALSE(again.receive_failure);
  EXPECT_TRUE(again.suppress_inbox_chrome);
  EXPECT_EQ(DissolvedLines(GroupMessages()), 1u) << "one line";
  EXPECT_EQ(Metadata().roster_epoch, 4u);
}

TEST_F(GroupDissolvedIngestTest, AGroupWeAreNotInIsNotEnded) {
  ASSERT_TRUE(bob_.roster.RemoveMember(kGroup, "account:bob"));  // left or removed before it arrived
  const RelayReceiveOutcome outcome = Receive(Statement(alice_));
  EXPECT_FALSE(outcome.receive_failure);
  EXPECT_FALSE(outcome.dissolved_group_thread_id);
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Active);
  EXPECT_EQ(DissolvedLines(GroupMessages()), 0u);
}

TEST_F(GroupDissolvedIngestTest, LateGroupTrafficAndInvitesAreDropped) {
  PendingGroupInvite waiting;
  waiting.invite_nonce = "waiting";
  waiting.group_id = kGroup;
  waiting.inviter_identity = alice_.Id();
  waiting.invitee_identity = "account:bob";
  waiting.status = InviteStatus::Pending;
  waiting.created_at = util::NowUnixMs();
  ASSERT_TRUE(bob_.roster.UpsertPendingInvite(waiting));

  const RelayReceiveOutcome outcome = Receive(Statement(alice_));
  ASSERT_EQ(outcome.voided_invite_cards.size(), 1u) << "the invite's card in the DM is closed";
  EXPECT_EQ(outcome.voided_invite_cards.front().second, "waiting");
  EXPECT_EQ(bob_.roster.LoadPendingInvite("waiting").value().value().status, InviteStatus::Expired);

  const RelayReceiveOutcome late =
      bob_.pipeline.ProcessEnvelope(GroupEnvelopeFrom("account:carol", kGroup, "account:bob"), "account:bob");
  EXPECT_EQ(late.decision, IngestDecision::HardReject);
  EXPECT_FALSE(late.persisted);
  EXPECT_FALSE(late.receive_failure) << "dropped on the roster, quietly";
  EXPECT_EQ(GroupMessages().size(), 2u);

  // A new invite to the dissolved group, even from its owner, is void.
  GroupInvitePayload invite;
  invite.group_id = kGroup;
  invite.group_title = "Weekend hike";
  invite.inviter_identity = alice_.Id();
  invite.invitee_identity = "account:bob";
  invite.invite_nonce = "after";
  invite.roster_epoch = 5;
  invite.expires_at = util::NowUnixMs() + 86400000;
  (void)bob_.pipeline.ProcessEnvelope(
      bob_.MakeSystemEnvelope("group_invite", GroupMembershipCodec::EncodeInvite(invite).value(), "Group invitation", ++seq_),
      "account:bob");
  EXPECT_FALSE(bob_.roster.LoadPendingInvite("after").value().has_value());
}

TEST_F(GroupDissolvedIngestTest, ADeletedChatStaysGoneAndTheMarkerSurvivesARestart) {
  ASSERT_TRUE(Receive(Statement(alice_)).dissolved_group_thread_id);

  // The user deletes the chat (InboxController::CloseThread: the thread goes, local membership is cleared).
  ASSERT_TRUE(bob_.store.DeleteThread(group_thread_.id).value());
  ASSERT_TRUE(bob_.roster.ClearGroupTarget(kGroup));
  const RelayReceiveOutcome again = Receive(Statement(alice_, true, 6));
  EXPECT_FALSE(again.dissolved_group_thread_id);
  EXPECT_FALSE(bob_.store.FindGroupThread(kGroup)->has_value()) << "a repeated dissolve brings no chat back";
  const RelayReceiveOutcome late =
      bob_.pipeline.ProcessEnvelope(GroupEnvelopeFrom("account:carol", kGroup, "account:bob"), "account:bob");
  EXPECT_FALSE(late.persisted);
  EXPECT_FALSE(bob_.store.FindGroupThread(kGroup)->has_value());

  // The roster keeps the ending (that it survives a restart is the app storage's to show).
  const auto metadata = bob_.roster.LoadMetadata(kGroup).value();
  ASSERT_TRUE(metadata);
  EXPECT_EQ(metadata->sync_state, GroupSyncState::Dissolved);
  EXPECT_TRUE(bob_.roster.ListMembers(kGroup).value().empty());
}


TEST_F(GroupDissolvedIngestTest, ARedeliveryFinishesADissolveThatCrashedBeforeItsLine) {
  // The group ended (one transaction) but the process died before its chat got the line.
  ASSERT_TRUE(EndGroupInRoster(bob_.roster, kGroup, alice_.Id(), 4));
  ASSERT_EQ(DissolvedLines(GroupMessages()), 0u);

  const RelayReceiveOutcome again = Receive(Statement(alice_));
  EXPECT_FALSE(again.receive_failure);
  ASSERT_TRUE(again.dissolved_group_thread_id) << "the line, and so the one notice, comes now";
  EXPECT_EQ(DissolvedLines(GroupMessages()), 1u);
  EXPECT_FALSE(Receive(Statement(alice_)).dissolved_group_thread_id) << "and only once";
  EXPECT_EQ(DissolvedLines(GroupMessages()), 1u);
}

TEST_F(GroupDissolvedIngestTest, AnAcceptThatCameAfterTheDissolveIsRefusedLikeAFullGroup) {
  PartyHarness owner("late_accept", "account:alice", "account:dave");
  GroupMetadata metadata;
  metadata.group_id = kGroup;
  metadata.owner_identity = "account:alice";
  metadata.roster_epoch = 3;
  ASSERT_TRUE(owner.roster.UpsertMetadata(metadata));
  GroupRosterMember alice;
  alice.member_identity = "account:alice";
  alice.role = MemberRole::Owner;
  ASSERT_TRUE(owner.roster.UpsertMember(kGroup, alice));
  PendingGroupInvite invite;
  invite.invite_nonce = "late";
  invite.group_id = kGroup;
  invite.inviter_identity = "account:alice";
  invite.invitee_identity = "account:dave";
  invite.status = InviteStatus::Pending;
  invite.created_at = util::NowUnixMs();
  invite.outbound = true;
  ASSERT_TRUE(owner.roster.UpsertPendingInvite(invite));
  ASSERT_TRUE(EndGroupInRoster(owner.roster, kGroup, "account:alice", 4));

  auto detail = GroupMembershipCodec::EncodeInviteResponse("late", kGroup);
  ASSERT_TRUE(detail);
  const RelayReceiveOutcome outcome = owner.pipeline.ProcessEnvelope(
      owner.MakeSystemEnvelope("group_invite_accept", *detail, "Accepted group invitation", 1), "account:alice");
  EXPECT_FALSE(outcome.publish_member_joined_group_id) << "no one joins a group that is over";
  ASSERT_TRUE(outcome.refuse_invite_group_id);
  EXPECT_EQ(outcome.refuse_invite_nonce, "late");
  EXPECT_EQ(outcome.refuse_invite_member, "account:dave");
  EXPECT_EQ(outcome.refuse_invite_reason, kInviteRefusedGroupDissolved);
  EXPECT_FALSE(outcome.receive_failure);
  EXPECT_TRUE(owner.roster.ListMembers(kGroup).value().empty());
}

TEST(GroupInviteIngestTest, AnInviteRefusedBecauseTheGroupWasDissolvedEndsTheInviteesGroupAndItsCard) {
  PartyHarness invitee("invitee_dissolved", "account:bob", "account:alice");
  PendingGroupInvite pending;
  pending.invite_nonce = "nonce-gone";
  pending.group_id = "group:hike";
  pending.inviter_identity = "account:alice";
  pending.invitee_identity = "account:bob";
  pending.status = InviteStatus::Accepted;  // bob accepted: its group exists, the owner's end beat the accept
  pending.created_at = util::NowUnixMs();
  ASSERT_TRUE(invitee.roster.UpsertPendingInvite(pending));
  GroupMetadata metadata;
  metadata.group_id = "group:hike";
  metadata.owner_identity = "account:alice";
  metadata.roster_epoch = 1;
  ASSERT_TRUE(invitee.roster.UpsertMetadata(metadata));
  for (const char* who : {"account:alice", "account:bob"}) {
    GroupRosterMember member;
    member.member_identity = who;
    ASSERT_TRUE(invitee.roster.UpsertMember("group:hike", member));
  }
  auto thread = invitee.store.FindOrCreateGroupThread("group:hike", "Weekend hike", {});
  ASSERT_TRUE(thread);

  auto detail = GroupMembershipCodec::EncodeInviteRefused("nonce-gone", "group:hike", kInviteRefusedGroupDissolved);
  ASSERT_TRUE(detail);
  const RelayReceiveOutcome outcome = invitee.pipeline.ProcessEnvelope(
      invitee.MakeSystemEnvelope("group_invite_refused", *detail, "Group invitation refused", 1), "account:bob");
  EXPECT_FALSE(outcome.receive_failure);
  auto after = invitee.roster.LoadMetadata("group:hike");
  ASSERT_TRUE(after && *after);
  EXPECT_EQ((*after)->sync_state, GroupSyncState::Dissolved) << "dissolved, not merely removed";
  EXPECT_TRUE(invitee.roster.ListMembers("group:hike").value().empty());
  ASSERT_TRUE(outcome.dissolved_group_thread_id);
  EXPECT_EQ(*outcome.dissolved_group_thread_id, thread->id);
  ASSERT_EQ(outcome.voided_invite_cards.size(), 1u) << "the invite's card ends";
  EXPECT_EQ(outcome.voided_invite_cards.front().second, "nonce-gone");
}

} // namespace

namespace pbr {
namespace {

// projects/privacy T3: a Blocked sender's direct messages (chat and call control alike) are dropped
// on receipt — before the thread, the inbox or a ring sees them.
// --- G017: the owner makes a member admin, or sets the preset ---------------------------------------------------

class GroupSettingIngestTest : public GroupDissolvedIngestTest {
protected:
  /** The owner's roster at epoch 4 with `bob_role` and `preset`. */
  GroupRosterStatement Roster(test::TestSigner& owner, const MemberRole bob_role,
                              const GroupPreset preset = GroupPreset::Open) {
    std::vector<GroupRosterStatementMember> rows{{owner.Id(), MemberRole::Owner, 1000},
                                                 {"account:bob", bob_role, 1000},
                                                 {"account:carol", MemberRole::Member, 1000}};
    return group_roster_statement::Sign(owner, owner.Key(), kGroup, 4, rows, {}, false, preset).value();
  }

  RelayReceiveOutcome ReceiveSetting(const std::string& control_type, const std::string& detail,
                                     const GroupRosterStatement& statement) {
    auto with = group_roster_sync_codec::WithStatement(detail, statement);
    EXPECT_TRUE(with);
    return bob_.pipeline.ProcessEnvelope(bob_.MakeSystemEnvelope(control_type, *with, "Group settings", ++seq_),
                                         "account:bob");
  }

  MemberRole RoleOf(const std::string& identity) {
    return bob_.roster.FindMember(kGroup, identity).value().value().role;
  }

  size_t Lines(const GroupMembershipControlType type) {
    size_t n = 0;
    for (const ThreadMessage& m : GroupMessages()) {
      const auto kind = GroupMembershipCodec::ControlTypeFromMessage(m);
      n += kind && *kind == type ? 1 : 0;
    }
    return n;
  }
};

TEST_F(GroupSettingIngestTest, TheOwnerMakingAMemberAdminTakesTheSignedRosterAndLeavesALine) {
  const auto detail =
      GroupMembershipCodec::EncodeMemberRoleChanged({kGroup, "account:bob", MemberRole::Admin, 4}).value();
  const RelayReceiveOutcome outcome = ReceiveSetting("member_role_changed", detail, Roster(alice_, MemberRole::Admin));
  EXPECT_FALSE(outcome.receive_failure);
  EXPECT_EQ(RoleOf("account:bob"), MemberRole::Admin);
  EXPECT_EQ(Metadata().roster_epoch, 4u);
  EXPECT_EQ(Lines(GroupMembershipControlType::MemberRoleChanged), 1u) << "who made whom an admin, in the group";
}

TEST_F(GroupSettingIngestTest, TheOwnersPresetIsTheGroups) {
  const auto detail = GroupMembershipCodec::EncodeGroupPresetChanged({kGroup, GroupPreset::Managed, 4}).value();
  const RelayReceiveOutcome outcome =
      ReceiveSetting("group_preset_changed", detail, Roster(alice_, MemberRole::Member, GroupPreset::Managed));
  EXPECT_FALSE(outcome.receive_failure);
  EXPECT_EQ(Metadata().policy.preset, GroupPreset::Managed);
  EXPECT_EQ(Lines(GroupMembershipControlType::GroupPresetChanged), 1u);
}

TEST_F(GroupSettingIngestTest, AnEventTheSignedRosterDoesNotBearOutIsRefused) {
  // The event says Carol was made admin; the roster the owner signed says otherwise.
  const auto detail =
      GroupMembershipCodec::EncodeMemberRoleChanged({kGroup, "account:carol", MemberRole::Admin, 4}).value();
  EXPECT_TRUE(ReceiveSetting("member_role_changed", detail, Roster(alice_, MemberRole::Member)).receive_failure);
  EXPECT_EQ(RoleOf("account:carol"), MemberRole::Member);
  EXPECT_EQ(Lines(GroupMembershipControlType::MemberRoleChanged), 0u);
}

TEST_F(GroupSettingIngestTest, OnlyTheOwnerMakesAdmins) {
  test::TestSigner carol;
  const auto detail =
      GroupMembershipCodec::EncodeMemberRoleChanged({kGroup, "account:bob", MemberRole::Admin, 4}).value();
  EXPECT_TRUE(ReceiveSetting("member_role_changed", detail, Roster(carol, MemberRole::Admin)).receive_failure)
      << "signed by someone who is not the owner we know";
  EXPECT_EQ(RoleOf("account:bob"), MemberRole::Member);
}

TEST(GroupSettingCodecTest, ARoleChangeNeverMakesAnOwner) {
  EXPECT_FALSE(GroupMembershipCodec::EncodeMemberRoleChanged({"group:g", "account:bob", MemberRole::Owner, 2}));
  EXPECT_FALSE(GroupMembershipCodec::DecodeMemberRoleChanged(
      R"({"group_id":"group:g","member_identity":"account:bob","role":"owner","roster_epoch":2})"));
  auto back = GroupMembershipCodec::DecodeMemberRoleChanged(
      GroupMembershipCodec::EncodeMemberRoleChanged({"group:g", "account:bob", MemberRole::Admin, 2}).value());
  ASSERT_TRUE(back);
  EXPECT_EQ(back->role, MemberRole::Admin);
  EXPECT_FALSE(GroupMembershipCodec::DecodeGroupPresetChanged(R"({"group_id":"group:g","preset":"anarchy"})"));
}

// ---- G018 wired: a member's or admin's change arrives as `group_change` ----

/**
 * Bob's device in the group of Alice (owner), Bob, Carol (admin) and Dave, holding Alice's statement; changes reach
 * it in DMs from Alice's device (the signer's own, or one handed on by a member).
 */
class GroupChangeIngestTest : public ::testing::Test {
protected:
  GroupChangeIngestTest()
      : bob_("changes_" + util::GenerateUuid(), test::Named("bob").Id(), test::Named("alice").Id()) {}

  void SetUp() override {
    GroupRosterStatement draft;
    draft.group_id = test::kGroupId;
    draft.roster_epoch = 5;
    draft.members = {test::Row(alice.Id(), MemberRole::Owner), test::Row(bob.Id()),
                     test::Row(carol.Id(), MemberRole::Admin), test::Row(dave.Id())};
    statement_ = test::SignedStatement(alice, draft);
    GroupMetadata metadata;
    metadata.group_id = test::kGroupId;
    metadata.owner_identity = alice.Id();
    metadata.roster_epoch = 5;
    ASSERT_TRUE(bob_.roster.UpsertMetadata(metadata));
    ASSERT_TRUE(*group_roster_sync::KeepStatement(bob_.roster, statement_));
    ASSERT_TRUE(group_roster_sync::ApplyView(bob_.roster, test::kGroupId, {}, util::NowUnixMs()));
    auto thread = bob_.store.FindOrCreateGroupThread(test::kGroupId, "Hikers", {});
    ASSERT_TRUE(thread);
    group_thread_ = *thread;
    ASSERT_TRUE(bob_.roster.UpsertGroupTarget(test::kGroupId, group_thread_.id, 1, 1));
  }

  /** `change` as first sent: with its signer's card (Bob's device keeps none of theirs). */
  static GroupChange Carded(GroupChange change, const test::Person& signer) {
    change.signer_card = signer.Card();
    return change;
  }

  RelayReceiveOutcome Receive(const GroupChange& change) {
    return bob_.pipeline.ProcessEnvelope(
        bob_.MakeSystemEnvelope("group_change", group_roster_sync_codec::EncodeChange(change), "Group members changed",
                                ++seq_),
        bob_.local_account);
  }

  bool IsMember(const test::Person& who) { return bob_.roster.IsMember(test::kGroupId, who.Id()).value(); }
  GroupMetadata Metadata() { return bob_.roster.LoadMetadata(test::kGroupId).value().value(); }

  std::vector<ThreadMessage> Lines(const GroupMembershipControlType type) {
    std::vector<ThreadMessage> out;
    const std::vector<ThreadMessage> all = bob_.store.GetMessages(group_thread_.id).value();
    for (const ThreadMessage& m : all) {
      if (GroupMembershipCodec::ControlTypeFromMessage(m) == type) {
        out.push_back(m);
      }
    }
    return out;
  }

  test::Person& alice = test::Named("alice");
  test::Person& bob = test::Named("bob");
  test::Person& carol = test::Named("carol");
  test::Person& dave = test::Named("dave");
  PartyHarness bob_;
  GroupRosterStatement statement_;
  Thread group_thread_;
  uint64_t seq_ = 0;
};

// The leave is the leaver's signed change now (G018): handed on by a member, it takes Dave off and the group's chat
// says so; an old app's member_left changes nothing.
TEST_F(GroupChangeIngestTest, ALeaveChangeReplacesMemberLeft) {
  const auto legacy = GroupMembershipCodec::EncodeMemberLeft(test::kGroupId, alice.Id(), 6);
  ASSERT_TRUE(legacy);
  (void)bob_.pipeline.ProcessEnvelope(bob_.MakeSystemEnvelope("member_left", *legacy, "Member left", ++seq_),
                                      bob_.local_account);
  EXPECT_TRUE(bob_.roster.IsMember(test::kGroupId, alice.Id()).value()) << "member_left is no longer read";

  const RelayReceiveOutcome outcome = Receive(Carded(dave.Leave(), dave));
  EXPECT_FALSE(outcome.receive_failure);
  EXPECT_TRUE(outcome.suppress_inbox_chrome) << "the DM is bookkeeping; the group's chat has the line";
  EXPECT_FALSE(IsMember(dave));
  const std::vector<ThreadMessage> left = Lines(GroupMembershipControlType::MemberLeft);
  ASSERT_EQ(left.size(), 1u);
  EXPECT_EQ(GroupMembershipCodec::DecodeMemberLeftFromMessage(left[0])->member_identity, dave.Id());
  EXPECT_EQ(bob_.roster.ListGroupChanges(test::kGroupId).value().size(), 1u) << "held until Alice folds it";
  EXPECT_FALSE(outcome.fold_group_id) << "folding is the owner's";

  (void)Receive(Carded(dave.Leave(), dave)); // a second copy
  EXPECT_EQ(Lines(GroupMembershipControlType::MemberLeft).size(), 1u) << "one line per change";
}

TEST_F(GroupChangeIngestTest, AForgedChangeIsRefused) {
  GroupChange forged = Carded(dave.Leave(), dave);
  forged.signature_b64 = carol.Leave().signature_b64;
  const RelayReceiveOutcome outcome = Receive(forged);
  EXPECT_TRUE(outcome.receive_failure);
  EXPECT_TRUE(IsMember(dave));
  EXPECT_TRUE(bob_.roster.ListGroupChanges(test::kGroupId).value().empty());
}

TEST_F(GroupChangeIngestTest, AChangeWhoseCardIsMissingIsKeptToProveLater) {
  const RelayReceiveOutcome outcome = Receive(dave.Leave()); // no card carried, none kept
  EXPECT_FALSE(outcome.receive_failure);
  EXPECT_TRUE(IsMember(dave)) << "not proved yet";
  EXPECT_EQ(bob_.roster.ListGroupChanges(test::kGroupId).value().size(), 1u);
  // The card arrives (kept by the app): the recompute takes Dave off.
  test::CardBook book;
  book.Hold(dave.Card());
  auto applied = group_roster_sync::ApplyView(bob_.roster, test::kGroupId, book.Lookup(), util::NowUnixMs());
  ASSERT_TRUE(applied);
  EXPECT_FALSE(IsMember(dave));
  EXPECT_EQ(applied->newly_applied.size(), 1u);
}

// G017 / G020: an admin's removal counts, and the line says who; a member's does not.
TEST_F(GroupChangeIngestTest, AnAdminRemovesAMemberAndAMemberCannot) {
  (void)Receive(Carded(dave.Remove(carol.Id()), dave));
  EXPECT_TRUE(IsMember(carol)) << "a member removes no one";
  EXPECT_TRUE(Lines(GroupMembershipControlType::MemberRemoved).empty());

  const RelayReceiveOutcome outcome = Receive(Carded(carol.Remove(dave.Id()), carol));
  EXPECT_FALSE(outcome.receive_failure);
  EXPECT_FALSE(IsMember(dave));
  const std::vector<ThreadMessage> removed = Lines(GroupMembershipControlType::MemberRemoved);
  ASSERT_EQ(removed.size(), 1u);
  auto payload = GroupMembershipCodec::DecodeMemberRemovedFromMessage(removed[0]);
  EXPECT_EQ(payload->member_identity, dave.Id());
  EXPECT_EQ(payload->removed_by, carol.Id());
}

// G020 3: removed by an admin, Bob's chat stays, read-only, and he is told who did it.
TEST_F(GroupChangeIngestTest, RemovedByAnAdminTheGroupGoesReadOnly) {
  const RelayReceiveOutcome outcome = Receive(Carded(carol.Remove(bob.Id()), carol));
  EXPECT_FALSE(outcome.receive_failure);
  EXPECT_FALSE(IsMember(bob));
  EXPECT_EQ(Metadata().sync_state, GroupSyncState::Removed);
  EXPECT_FALSE(group_roster_sync::MaySend(Metadata()));
  EXPECT_EQ(outcome.about_us, RelayReceiveOutcome::AboutUs::Removed);
  EXPECT_EQ(outcome.about_us_group_thread_id, group_thread_.id);
  const auto removed = Lines(GroupMembershipControlType::MemberRemoved);
  ASSERT_EQ(removed.size(), 1u);
  EXPECT_EQ(GroupMembershipCodec::DecodeMemberRemovedFromMessage(removed[0])->removed_by, carol.Id());
}

// A change for a group we are no longer in, or handed on by someone outside it, does nothing.
TEST_F(GroupChangeIngestTest, OnlyTheGroupsMembersHandChangesOn) {
  PartyHarness outsider("changes_outsider_" + util::GenerateUuid(), bob.Id(), "account:mallory");
  GroupMetadata metadata;
  metadata.group_id = test::kGroupId;
  metadata.owner_identity = alice.Id();
  metadata.roster_epoch = 5;
  ASSERT_TRUE(outsider.roster.UpsertMetadata(metadata));
  ASSERT_TRUE(*group_roster_sync::KeepStatement(outsider.roster, statement_));
  ASSERT_TRUE(group_roster_sync::ApplyView(outsider.roster, test::kGroupId, {}, util::NowUnixMs()));
  const RelayReceiveOutcome outcome = outsider.pipeline.ProcessEnvelope(
      outsider.MakeSystemEnvelope("group_change", group_roster_sync_codec::EncodeChange(Carded(dave.Leave(), dave)),
                                  "Group members changed", 1),
      bob.Id());
  EXPECT_TRUE(outcome.receive_failure) << "Mallory is not in the group";
  EXPECT_TRUE(outsider.roster.IsMember(test::kGroupId, dave.Id()).value());
}

// The owner's device folds what arrives (debounced by the app): the outcome asks for it.
TEST(GroupChangeOwnerIngestTest, TheOwnerIsAskedToFoldWhatArrives) {
  test::Person& alice = test::Named("alice");
  test::Person& bob = test::Named("bob");
  test::Person& dave = test::Named("dave");
  PartyHarness owner("changes_owner_" + util::GenerateUuid(), alice.Id(), bob.Id());
  GroupRosterStatement draft;
  draft.group_id = test::kGroupId;
  draft.roster_epoch = 5;
  draft.members = {test::Row(alice.Id(), MemberRole::Owner), test::Row(bob.Id()), test::Row(dave.Id())};
  const GroupRosterStatement statement = test::SignedStatement(alice, draft);
  GroupMetadata metadata;
  metadata.group_id = test::kGroupId;
  metadata.owner_identity = alice.Id();
  metadata.roster_epoch = 5;
  ASSERT_TRUE(owner.roster.UpsertMetadata(metadata));
  ASSERT_TRUE(*group_roster_sync::KeepStatement(owner.roster, statement));
  ASSERT_TRUE(group_roster_sync::ApplyView(owner.roster, test::kGroupId, {}, util::NowUnixMs()));
  GroupChange leave = dave.Leave();
  leave.signer_card = dave.Card();
  const RelayReceiveOutcome outcome = owner.pipeline.ProcessEnvelope(
      owner.MakeSystemEnvelope("group_change", group_roster_sync_codec::EncodeChange(leave), "Group members changed",
                               1),
      alice.Id());
  EXPECT_FALSE(outcome.receive_failure);
  ASSERT_TRUE(outcome.fold_group_id);
  EXPECT_EQ(*outcome.fold_group_id, test::kGroupId);
  EXPECT_FALSE(owner.roster.IsMember(test::kGroupId, dave.Id()).value());
}

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
