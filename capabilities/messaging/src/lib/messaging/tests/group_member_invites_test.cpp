// G019 / G020 (o4): members invite per the preset and their device signs the add; the inviter's checks of an accept;
// a re-admit of someone removed (only the owner or an admin); a member's rename; the join bundle's changes; and the
// bulk remedies' targets ("Remove everyone {name} added", "Re-invite everyone {name} removed").
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/GroupRosterSyncCodec.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/tests/GroupChangeTestKit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using test::kGroupId;
using test::kNowMs;
using test::Named;
using test::Person;
using test::Row;
namespace grc = group_change;
namespace grs = group_roster_sync;

/** One device's store for the group of Alice (owner), Bob (admin), Carol and Dave (members); Erin and Frank outside. */
class GroupMemberInvitesTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (Person* p : {&alice, &bob, &carol, &dave, &erin, &frank}) {
      book.Hold(p->Card());
    }
    store_ = std::make_unique<InMemoryDeliveryStore>();
    ASSERT_TRUE(store_->ListThreads());
    roster_ = std::make_unique<InMemoryGroupRosterStore>(*store_);
  }

  void TearDown() override {
    roster_.reset();
    store_.reset();
  }

  GroupRosterStatement Statement(const GroupPreset preset = GroupPreset::Open,
                                 const std::function<void(GroupRosterStatement&)>& edit = {}) {
    GroupRosterStatement draft;
    draft.roster_epoch = 5;
    draft.members = {Row(alice.Id(), MemberRole::Owner), Row(bob.Id(), MemberRole::Admin), Row(carol.Id()),
                     Row(dave.Id())};
    draft.preset = preset;
    draft.title = "Hikers";
    draft.title_version = 1;
    if (edit) {
      edit(draft);
    }
    return test::SignedStatement(alice, draft);
  }

  /** This device holds `statement` (and its preset), as a roster event left it. */
  void Seed(const GroupRosterStatement& statement, IGroupRosterStore* store = nullptr) {
    IGroupRosterStore& roster = store ? *store : *roster_;
    GroupMetadata metadata;
    metadata.group_id = kGroupId;
    metadata.owner_identity = alice.Id();
    metadata.roster_epoch = statement.roster_epoch;
    metadata.title = statement.title;
    metadata.title_version = statement.title_version;
    ASSERT_TRUE(roster.UpsertMetadata(metadata));
    ASSERT_TRUE(*grs::KeepStatement(roster, statement));
    ASSERT_TRUE(grs::ApplyView(roster, kGroupId, Cards(), kNowMs));
  }

  GroupCardLookup Cards() const { return book.Lookup(); }

  /** `who`'s add of `member` as its device would make it here (DraftAdd), signed by that device. */
  Roe<GroupChange> DraftAddBy(const Person& who, const std::string& member, const std::string& nonce = "invite-1") {
    auto draft = grs::DraftAdd(*roster_, kGroupId, who.Id(), who.Device().peer_id, member, nonce, Cards(), kNowMs);
    if (!draft) {
      return draft.error();
    }
    return who.Signed(std::move(*draft));
  }

  grs::ChangeReceived Receive(const GroupChange& change, IGroupRosterStore* store = nullptr) {
    auto received = grs::ReceiveChange(store ? *store : *roster_, change, Cards(), kNowMs);
    EXPECT_TRUE(received) << received.error().message;
    return received ? *received : grs::ChangeReceived{};
  }

  bool IsMember(const std::string& identity, IGroupRosterStore* store = nullptr) {
    auto member = (store ? *store : *roster_).IsMember(kGroupId, identity);
    return member && *member;
  }

  std::string AddedBy(const std::string& identity) {
    auto row = roster_->FindMember(kGroupId, identity);
    return row && *row ? (*row)->added_by : std::string();
  }

  /** An invite this device (`inviter`) sent to `invitee`, waiting. */
  void RecordInvite(const Person& inviter, const std::string& invitee, const std::string& nonce = "invite-1") {
    PendingGroupInvite pending;
    pending.invite_nonce = nonce;
    pending.group_id = kGroupId;
    pending.inviter_identity = inviter.Id();
    pending.invitee_identity = invitee;
    pending.owner_identity = alice.Id();
    pending.status = InviteStatus::Pending;
    pending.expires_at = kNowMs + kGroupInviteTtlMs;
    pending.created_at = kNowMs;
    pending.outbound = true;
    ASSERT_TRUE(roster_->UpsertPendingInvite(pending));
  }

  Person& alice = Named("alice");
  Person& bob = Named("bob");
  Person& carol = Named("carol");
  Person& dave = Named("dave");
  Person& erin = Named("erin");
  Person& frank = Named("frank");
  test::CardBook book;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<InMemoryGroupRosterStore> roster_;
};

// ---- Who may invite (G017, G019 1) ----

TEST_F(GroupMemberInvitesTest, AnOpenGroupsMemberSignsTheAddAndItCountsWithWhoBroughtThem) {
  Seed(Statement());
  auto add = DraftAddBy(carol, erin.Id());
  ASSERT_TRUE(add) << add.error().message;
  EXPECT_EQ(add->kind, GroupChangeKind::Add);
  EXPECT_EQ(add->member, erin.Id());
  EXPECT_EQ(add->invite_nonce, "invite-1");
  EXPECT_TRUE(add->readmits.empty());
  EXPECT_EQ(add->base_epoch, 5u) << "made against the statement Carol holds";

  const grs::ChangeReceived received = Receive(*add);
  EXPECT_EQ(received.held, grs::ChangeHeld::Proved);
  EXPECT_TRUE(IsMember(erin.Id()));
  EXPECT_EQ(AddedBy(erin.Id()), carol.Id()) << "the member list shows who added them (G019 5)";
  EXPECT_EQ(received.applied.joined, std::vector<std::string>{erin.Id()});
}

TEST_F(GroupMemberInvitesTest, AManagedGroupsMemberMayNotInviteButAnAdminMay) {
  Seed(Statement(GroupPreset::Managed));
  auto by_member = DraftAddBy(carol, erin.Id());
  ASSERT_FALSE(by_member);
  EXPECT_EQ(by_member.error().message, kGroupInviteNotAllowedError);

  auto by_admin = DraftAddBy(bob, erin.Id());
  ASSERT_TRUE(by_admin) << by_admin.error().message;
  Receive(*by_admin);
  EXPECT_TRUE(IsMember(erin.Id()));

  // A member's add made anyway (a modified client) does not count here (G017 4, R3).
  Receive(carol.Add(frank.Id(), "invite-2"));
  EXPECT_FALSE(IsMember(frank.Id()));
}

TEST_F(GroupMemberInvitesTest, NoAddForSomeoneAlreadyInOrForOurselves) {
  Seed(Statement());
  auto again = DraftAddBy(carol, dave.Id());
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().message, kGroupAlreadyMemberError);
  EXPECT_FALSE(DraftAddBy(carol, carol.Id()));
}

// ---- The inviter's checks of an accept (G019 2, G014) ----

TEST_F(GroupMemberInvitesTest, AnAcceptOfAMembersInviteIsTheMembersToSign) {
  Seed(Statement());
  RecordInvite(carol, erin.Id());
  auto accepted = ApplyInviteAcceptToRoster(*roster_, "invite-1", erin.Id(), kGroupId, carol.Id(), kNowMs);
  ASSERT_TRUE(accepted) << accepted.error().message;
  EXPECT_EQ(*accepted, InviteAccepted::SignAdd);
  EXPECT_FALSE(IsMember(erin.Id())) << "nothing written until the add is signed";
  EXPECT_EQ(roster_->LoadPendingInvite("invite-1").value()->status, InviteStatus::Pending);

  EXPECT_FALSE(ApplyInviteAcceptToRoster(*roster_, "invite-1", frank.Id(), kGroupId, carol.Id(), kNowMs))
      << "the accept must come from its invitee";
  EXPECT_FALSE(ApplyInviteAcceptToRoster(*roster_, "invite-1", erin.Id(), "group:other", carol.Id(), kNowMs))
      << "for this group";
  EXPECT_FALSE(ApplyInviteAcceptToRoster(*roster_, "missing", erin.Id(), kGroupId, carol.Id(), kNowMs));
}

TEST_F(GroupMemberInvitesTest, AnAcceptAfterTheGroupWentManagedIsRefused) {
  Seed(Statement());
  RecordInvite(carol, erin.Id());
  Seed(Statement(GroupPreset::Managed, [](GroupRosterStatement& d) { d.roster_epoch = 6; }));
  auto accepted = ApplyInviteAcceptToRoster(*roster_, "invite-1", erin.Id(), kGroupId, carol.Id(), kNowMs);
  ASSERT_FALSE(accepted);
  EXPECT_EQ(accepted.error().message, kGroupInviteNotAllowedError) << "the invitee gets group_invite_refused";
}

TEST_F(GroupMemberInvitesTest, AnAcceptWhenTheGroupIsFullIsRefused) {
  Seed(Statement(GroupPreset::Open, [](GroupRosterStatement& d) {
    for (int i = 0; d.members.size() < kMaxGroupMembers; ++i) {
      d.members.push_back(Row("account:filler" + std::to_string(i)));
    }
  }));
  RecordInvite(carol, erin.Id());
  auto accepted = ApplyInviteAcceptToRoster(*roster_, "invite-1", erin.Id(), kGroupId, carol.Id(), kNowMs);
  ASSERT_FALSE(accepted);
  EXPECT_EQ(accepted.error().message, kGroupFullError);
  EXPECT_EQ(roster_->LoadPendingInvite("invite-1").value()->status, InviteStatus::Expired);

  // An add signed anyway does not count past 50 (R5): whoever signed it refuses its invitee.
  const grs::ChangeReceived received = Receive(carol.Add(erin.Id()));
  EXPECT_FALSE(IsMember(erin.Id()));
  ASSERT_TRUE(received.applied.view);
  const GroupChangeVerdict* verdict = received.applied.view->Verdict(grc::Id(carol.Add(erin.Id())));
  ASSERT_NE(verdict, nullptr);
  EXPECT_EQ(verdict->reason, GroupChangeReason::GroupFull);
}

TEST_F(GroupMemberInvitesTest, TheOwnersOwnAcceptAddsAtOnceAndSaysWhoAdded) {
  Seed(Statement());
  RecordInvite(alice, erin.Id());
  auto accepted = ApplyInviteAcceptToRoster(*roster_, "invite-1", erin.Id(), kGroupId, alice.Id(), kNowMs);
  ASSERT_TRUE(accepted) << accepted.error().message;
  EXPECT_EQ(*accepted, InviteAccepted::Added);
  EXPECT_TRUE(IsMember(erin.Id()));
  EXPECT_EQ(AddedBy(erin.Id()), alice.Id());
  auto again = ApplyInviteAcceptToRoster(*roster_, "invite-1", erin.Id(), kGroupId, alice.Id(), kNowMs);
  ASSERT_TRUE(again);
  EXPECT_EQ(*again, InviteAccepted::Already);
}

// ---- Re-admit (G020 5, R2) ----

TEST_F(GroupMemberInvitesTest, AnAdminReadmitsSomeoneRemovedAndAMemberMayNot) {
  Seed(Statement());
  auto removal = grs::DraftRemove(*roster_, kGroupId, bob.Id(), bob.Device().peer_id, dave.Id(), kNowMs);
  ASSERT_TRUE(removal) << removal.error().message;
  const GroupChange remove = bob.Signed(std::move(*removal));
  Receive(remove);
  ASSERT_FALSE(IsMember(dave.Id()));
  auto entry = grs::RemovedEntry(*roster_, kGroupId, dave.Id(), Cards(), kNowMs);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->removal, grc::Id(remove));

  auto by_member = DraftAddBy(carol, dave.Id());
  ASSERT_FALSE(by_member);
  EXPECT_EQ(by_member.error().message, kGroupInviteRemovedError);
  // Made anyway, it is void (R2): a member's add of someone a removal took out.
  Receive(carol.Add(dave.Id(), "invite-x"));
  EXPECT_FALSE(IsMember(dave.Id()));

  auto readmit = DraftAddBy(bob, dave.Id(), "invite-2");
  ASSERT_TRUE(readmit) << readmit.error().message;
  EXPECT_EQ(readmit->readmits, grc::Id(remove)) << "the add names the removal it lifts";
  Receive(*readmit);
  EXPECT_TRUE(IsMember(dave.Id()));
  EXPECT_FALSE(grs::RemovedEntry(*roster_, kGroupId, dave.Id(), Cards(), kNowMs));
}

TEST_F(GroupMemberInvitesTest, AReadmitNamesTheStatementsRemovalOnceItIsFolded) {
  Seed(Statement(GroupPreset::Open, [&](GroupRosterStatement& d) {
    d.members.pop_back(); // Dave out, folded
    d.removed = {{.identity = dave.Id(), .removal = "folded-removal", .removed_by = alice.Id()}};
  }));
  auto readmit = DraftAddBy(bob, dave.Id());
  ASSERT_TRUE(readmit) << readmit.error().message;
  EXPECT_EQ(readmit->readmits, "folded-removal");
  Receive(*readmit);
  EXPECT_TRUE(IsMember(dave.Id()));
}

// ---- A member's rename (G017) ----

TEST_F(GroupMemberInvitesTest, AnOpenGroupsMemberRenamesWithTheNextTitleVersion) {
  Seed(Statement());
  auto draft = grs::DraftRename(*roster_, kGroupId, carol.Id(), carol.Device().peer_id, "Bikers", Cards(), kNowMs);
  ASSERT_TRUE(draft) << draft.error().message;
  EXPECT_EQ(draft->title_version, 2u) << "one past the title the roster holds";
  const grs::ChangeReceived received = Receive(carol.Signed(std::move(*draft)));
  ASSERT_TRUE(received.applied.new_title);
  EXPECT_EQ(*received.applied.new_title, "Bikers") << "the chat's title follows";
  const GroupMetadata metadata = roster_->LoadMetadata(kGroupId).value().value();
  EXPECT_EQ(metadata.title, "Bikers");
  EXPECT_EQ(metadata.title_version, 2u);

  auto next = grs::DraftRename(*roster_, kGroupId, dave.Id(), dave.Device().peer_id, "Walkers", Cards(), kNowMs);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->title_version, 3u);
}

TEST_F(GroupMemberInvitesTest, AManagedGroupsMemberMayNotRename) {
  Seed(Statement(GroupPreset::Managed));
  auto by_member = grs::DraftRename(*roster_, kGroupId, carol.Id(), carol.Device().peer_id, "Bikers", Cards(), kNowMs);
  ASSERT_FALSE(by_member);
  EXPECT_EQ(by_member.error().message, kGroupRenameNotAllowedError);
  EXPECT_TRUE(grs::DraftRename(*roster_, kGroupId, bob.Id(), bob.Device().peer_id, "Bikers", Cards(), kNowMs));
  // Signed anyway, a member's rename does not count (R3).
  Receive(carol.Rename("Bikers", 2));
  EXPECT_EQ(roster_->LoadMetadata(kGroupId).value().value().title, "Hikers");
}

// ---- The join bundle (G018 item 6, G019 2) ----

TEST_F(GroupMemberInvitesTest, TheBundleCarriesTheAddFirstAndEachCardOnce) {
  Seed(Statement());
  GroupChange leave = dave.Leave();
  leave.signer_card = dave.Card();
  Receive(leave);
  GroupChange add = carol.Add(erin.Id());
  add.signer_card = carol.Card();
  GroupChange rename = carol.Rename("Bikers", 2);
  rename.signer_card = carol.Card();
  Receive(add);
  Receive(rename);

  const group_roster_sync_codec::CarriedChanges carried = grs::ChangesForNewcomer(*roster_, kGroupId, grc::Id(add));
  ASSERT_EQ(carried.changes.size(), 3u);
  EXPECT_EQ(grc::Id(carried.changes.front()), grc::Id(add)) << "the add that brings them in leads";
  EXPECT_FALSE(carried.more);
  size_t carol_cards = 0;
  for (const GroupChange& c : carried.changes) {
    carol_cards += c.signer == carol.Id() && c.signer_card ? 1 : 0;
  }
  EXPECT_EQ(carol_cards, 1u) << "a signer's card rides the bundle once";
}

TEST_F(GroupMemberInvitesTest, TheNewcomerChecksTheBundleItselfAndTrustsNoOne) {
  Seed(Statement());
  GroupChange add = carol.Add(erin.Id());
  add.signer_card = carol.Card();

  // Erin's device: the group as accepting the invite left it (the owner the invite named), nothing proved yet.
  InMemoryGroupRosterStore newcomer(*store_);
  GroupMetadata metadata;
  metadata.group_id = kGroupId;
  metadata.owner_identity = alice.Id();
  metadata.roster_epoch = 5;
  ASSERT_TRUE(newcomer.UpsertMetadata(metadata));
  const test::CardBook empty;
  const GroupCardLookup none = empty.Lookup();

  // A statement Carol made up (signed by her as if the owner) does not verify for the owner the invite named.
  GroupRosterStatement forged_draft = Statement();
  forged_draft.members.push_back(Row(erin.Id()));
  forged_draft.members[0].identity = carol.Id();
  forged_draft.members[2].identity = alice.Id();
  forged_draft.members[2].role = MemberRole::Member;
  const GroupRosterStatement forged = test::SignedStatement(carol, forged_draft);
  EXPECT_FALSE(grs::KeepStatement(newcomer, forged)) << "the statement must be the owner's";

  // A change whose signature fails is never held.
  GroupChange tampered = add;
  tampered.member = frank.Id();
  ASSERT_TRUE(grs::HoldCarried(newcomer, {tampered}, none, kNowMs, true));
  EXPECT_TRUE(newcomer.ListGroupChanges(kGroupId).value().empty());

  // The real bundle: the owner's statement and Carol's add, each proving itself — Erin is in.
  ASSERT_TRUE(*grs::KeepStatement(newcomer, Statement()));
  ASSERT_TRUE(grs::HoldCarried(newcomer, {add}, none, kNowMs, true));
  auto applied = grs::ApplyView(newcomer, kGroupId, none, kNowMs);
  ASSERT_TRUE(applied);
  ASSERT_TRUE(applied->view);
  EXPECT_TRUE(applied->view->Lists(erin.Id()));
  EXPECT_TRUE(applied->newly_applied.empty()) << "what came before the join gets no line on the newcomer's side";
  EXPECT_EQ(newcomer.FindMember(kGroupId, erin.Id()).value()->added_by, carol.Id());
}

// ---- The bulk remedies (G020 6) ----

TEST_F(GroupMemberInvitesTest, RemoveEveryoneAMemberAddedTargetsWhomTheRoleMayRemove) {
  Seed(Statement());
  Receive(carol.Add(erin.Id(), "invite-1"));
  Receive(carol.Add(frank.Id(), "invite-2"));
  ASSERT_TRUE(IsMember(erin.Id()) && IsMember(frank.Id()));

  const std::vector<std::string> targets = grs::RemovableAddedBy(*roster_, kGroupId, bob.Id(), carol.Id());
  std::vector<std::string> expected{erin.Id(), frank.Id()};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(targets, expected) << "Carol stays: only the people she added";
  EXPECT_TRUE(grs::RemovableAddedBy(*roster_, kGroupId, dave.Id(), carol.Id()).empty()) << "a member removes no one";

  // Bob's one removal each takes them out.
  for (const std::string& target : targets) {
    auto draft = grs::DraftRemove(*roster_, kGroupId, bob.Id(), bob.Device().peer_id, target, kNowMs);
    ASSERT_TRUE(draft) << draft.error().message;
    Receive(bob.Signed(std::move(*draft)));
  }
  EXPECT_FALSE(IsMember(erin.Id()));
  EXPECT_FALSE(IsMember(frank.Id()));
  EXPECT_TRUE(IsMember(carol.Id()));
}

TEST_F(GroupMemberInvitesTest, AnAdminsBulkRemoveLeavesTheAdminsSomeoneAdded) {
  Seed(Statement(GroupPreset::Open, [&](GroupRosterStatement& d) {
    d.members.push_back(Row(erin.Id(), MemberRole::Admin, carol.Id()));
    d.members.push_back(Row(frank.Id(), MemberRole::Member, carol.Id()));
  }));
  EXPECT_EQ(grs::RemovableAddedBy(*roster_, kGroupId, bob.Id(), carol.Id()), std::vector<std::string>{frank.Id()})
      << "only the owner removes admins";
  std::vector<std::string> both{erin.Id(), frank.Id()};
  std::sort(both.begin(), both.end());
  EXPECT_EQ(grs::RemovableAddedBy(*roster_, kGroupId, alice.Id(), carol.Id()), both);
}

TEST_F(GroupMemberInvitesTest, ReinviteEveryoneAnAdminRemovedFindsThoseStillOut) {
  Seed(Statement(GroupPreset::Open, [&](GroupRosterStatement& d) {
    d.members.pop_back(); // Dave, removed by Bob and folded
    d.removed = {{.identity = dave.Id(), .removal = "r-dave", .removed_by = bob.Id()},
                 {.identity = erin.Id(), .removal = "r-erin", .removed_by = alice.Id()}};
  }));
  auto removal = grs::DraftRemove(*roster_, kGroupId, bob.Id(), bob.Device().peer_id, carol.Id(), kNowMs);
  ASSERT_TRUE(removal);
  Receive(bob.Signed(std::move(*removal))); // and Carol, not folded yet

  std::vector<std::string> expected{carol.Id(), dave.Id()};
  std::sort(expected.begin(), expected.end());
  std::vector<std::string> found = grs::RemovedBy(*roster_, kGroupId, bob.Id(), Cards(), kNowMs);
  std::sort(found.begin(), found.end());
  EXPECT_EQ(found, expected) << "Erin was the owner's removal";
}

// ---- Wire (G019 3) ----

TEST(GroupInviteWireTest, TheInviteCarriesTheCountAndTheOwnerNeverTheMembers) {
  GroupInvitePayload invite;
  invite.group_id = kGroupId;
  invite.group_title = "Hikers";
  invite.inviter_identity = "account:carol";
  invite.invitee_identity = "account:erin";
  invite.invite_nonce = "n1";
  invite.actor_role = MemberRole::Member;
  invite.owner_identity = "account:alice";
  invite.member_count = 4;
  const std::string detail = GroupMembershipCodec::EncodeInvite(invite).value();
  EXPECT_EQ(detail.find("members\""), std::string::npos) << "no member list before a join (G019 3)";
  auto decoded = GroupMembershipCodec::DecodeInvite(detail);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->member_count, 4u);
  EXPECT_EQ(decoded->owner_identity, "account:alice");
  EXPECT_EQ(decoded->Owner(), "account:alice");
  EXPECT_EQ(decoded->actor_role, MemberRole::Member);

  invite.owner_identity = invite.inviter_identity = "account:alice";
  const std::string by_owner = GroupMembershipCodec::EncodeInvite(invite).value();
  EXPECT_EQ(by_owner.find("owner_identity"), std::string::npos) << "the owner's own invite names none (additive)";
  EXPECT_EQ(GroupMembershipCodec::DecodeInvite(by_owner)->Owner(), "account:alice");
}

TEST(GroupInviteWireTest, AChangeCarriesItsNewcomerAndTheBundleItsChanges) {
  Person& carol = Named("carol");
  GroupChange add = carol.Add("account:erin");
  Object card;
  card.set("account_id", "account:erin");
  const std::string detail = group_roster_sync_codec::EncodeChange(
      add, group_roster_sync_codec::ChangeNewcomer{.member_identity = "account:erin", .card = card});
  auto newcomer = group_roster_sync_codec::NewcomerIn(detail);
  ASSERT_TRUE(newcomer);
  EXPECT_EQ(newcomer->member_identity, "account:erin");
  EXPECT_TRUE(newcomer->card);
  EXPECT_EQ(grc::Id(group_roster_sync_codec::DecodeChange(detail).value()), grc::Id(add));
  EXPECT_FALSE(group_roster_sync_codec::NewcomerIn(group_roster_sync_codec::EncodeChange(add)));

  auto with = group_roster_sync_codec::WithChanges(R"({"group_id":"group:hikers"})", {.changes = {add}, .more = true});
  ASSERT_TRUE(with);
  const group_roster_sync_codec::CarriedChanges carried = group_roster_sync_codec::ChangesIn(*with, kGroupId);
  ASSERT_EQ(carried.changes.size(), 1u);
  EXPECT_TRUE(carried.more);
  EXPECT_TRUE(group_roster_sync_codec::ChangesIn(*with, "group:other").changes.empty());

  GroupMembershipCodec::MemberJoinedPayload joined{.group_id = kGroupId, .member_identity = "account:erin"};
  joined.added_by = "account:carol";
  joined.invite_nonce = "n1";
  auto decoded = GroupMembershipCodec::DecodeMemberJoined(GroupMembershipCodec::EncodeMemberJoined(joined).value());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->added_by, "account:carol");
  EXPECT_EQ(decoded->invite_nonce, "n1");
}

} // namespace
} // namespace pbr
