// G018 wired (o2): the one apply path (the held statement plus the held changes → the roster store), how a received
// change is held, the producers' drafts (leave, removal), the owner's fold, the change floor, the state a group copy
// carries and the catch-up it starts, and the member check reply carrying the changes the asker lacks.
#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/GroupChangeLines.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/tests/GroupChangeTestKit.h"

#include "common/ValueJson.h"

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

/** One device's roster store for the group of Alice (owner), Bob (admin), Carol and Dave (members). */
class GroupChangesWiredTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (Person* p : {&alice, &bob, &carol, &dave, &erin}) {
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

  GroupRosterStatement Statement(const std::function<void(GroupRosterStatement&)>& edit = {}) {
    GroupRosterStatement draft;
    draft.roster_epoch = 5;
    draft.members = {Row(alice.Id(), MemberRole::Owner), Row(bob.Id(), MemberRole::Admin), Row(carol.Id()),
                     Row(dave.Id())};
    draft.title = "Hikers";
    draft.title_version = 1;
    if (edit) {
      edit(draft);
    }
    return test::SignedStatement(alice, draft);
  }

  /** The device holds `statement`, as a roster event left it. */
  void Seed(IGroupRosterStore& roster, const GroupRosterStatement& statement) {
    GroupMetadata metadata;
    metadata.group_id = kGroupId;
    metadata.owner_identity = alice.Id();
    metadata.roster_epoch = statement.roster_epoch;
    ASSERT_TRUE(roster.UpsertMetadata(metadata));
    ASSERT_TRUE(*grs::KeepStatement(roster, statement));
    ASSERT_TRUE(grs::ApplyView(roster, kGroupId, Cards(), kNowMs));
  }

  GroupCardLookup Cards() const { return book.Lookup(); }

  grs::ChangeReceived Receive(const GroupChange& change, IGroupRosterStore* roster = nullptr) {
    auto received = grs::ReceiveChange(roster ? *roster : *roster_, change, Cards(), kNowMs);
    EXPECT_TRUE(received) << received.error().message;
    return received ? *received : grs::ChangeReceived{};
  }

  std::vector<std::string> Members(IGroupRosterStore* roster = nullptr) {
    std::vector<std::string> out;
    const std::vector<GroupRosterMember> members = (roster ? *roster : *roster_).ListMembers(kGroupId).value();
    for (const GroupRosterMember& m : members) {
      out.push_back(m.member_identity);
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  static std::vector<std::string> Sorted(std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  size_t Held(IGroupRosterStore* roster = nullptr) {
    return (roster ? *roster : *roster_).ListGroupChanges(kGroupId).value().size();
  }

  GroupMetadata Metadata(IGroupRosterStore* roster = nullptr) {
    return (roster ? *roster : *roster_).LoadMetadata(kGroupId).value().value();
  }

  Person& alice = Named("alice");
  Person& bob = Named("bob");
  Person& carol = Named("carol");
  Person& dave = Named("dave");
  Person& erin = Named("erin");
  test::CardBook book;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<InMemoryGroupRosterStore> roster_;
};

// ---- The one apply path ----

TEST_F(GroupChangesWiredTest, TheRosterIsTheStatementPlusTheChangesHeld) {
  Seed(*roster_, Statement());
  const GroupChange add = carol.Add(erin.Id());
  const grs::ChangeReceived received = Receive(add);
  EXPECT_EQ(received.held, grs::ChangeHeld::Proved);
  EXPECT_EQ(Members(), Sorted({alice.Id(), bob.Id(), carol.Id(), dave.Id(), erin.Id()}));
  const GroupRosterMember erin_row = roster_->FindMember(kGroupId, erin.Id()).value().value();
  EXPECT_EQ(erin_row.added_by, carol.Id()) << "who brought whom, in the store (G019)";
  EXPECT_EQ(erin_row.joined_at, add.at);
  EXPECT_EQ(erin_row.role, MemberRole::Member);
  EXPECT_EQ(received.applied.joined, std::vector<std::string>{erin.Id()});
  ASSERT_EQ(received.applied.newly_applied.size(), 1u) << "its line is the caller's to write, once";
  EXPECT_EQ(Held(), 1u) << "held until the owner folds it";

  auto again = grs::ApplyView(*roster_, kGroupId, Cards(), kNowMs);
  ASSERT_TRUE(again);
  EXPECT_TRUE(again->newly_applied.empty()) << "a recompute writes no second line";
  EXPECT_EQ(Members().size(), 5u);
}

TEST_F(GroupChangesWiredTest, WithNoStatementTheChangesWait) {
  GroupMetadata metadata;
  metadata.group_id = kGroupId;
  metadata.owner_identity = alice.Id();
  ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  const grs::ChangeReceived received = Receive(carol.Add(erin.Id()));
  EXPECT_EQ(received.held, grs::ChangeHeld::Proved);
  EXPECT_FALSE(received.applied.view) << "nothing to count it against";
  EXPECT_TRUE(Members().empty());
  EXPECT_EQ(Held(), 1u);
}

TEST_F(GroupChangesWiredTest, AForgedChangeIsRefusedAndNeverHeld) {
  Seed(*roster_, Statement());
  GroupChange forged = carol.Add(erin.Id());
  forged.signature_b64 = dave.Add(erin.Id()).signature_b64;
  EXPECT_EQ(Receive(forged).held, grs::ChangeHeld::Refused);
  EXPECT_EQ(Held(), 0u);
  EXPECT_FALSE(roster_->FindMember(kGroupId, erin.Id()).value());

  GroupChange elsewhere = carol.Add(erin.Id());
  elsewhere.group_id = "group:not-ours";
  EXPECT_EQ(Receive(elsewhere).held, grs::ChangeHeld::Refused) << "a group we do not hold";
}

TEST_F(GroupChangesWiredTest, AChangeWithNoCardWaitsForItsCard) {
  Seed(*roster_, Statement());
  book.cards.erase(carol.Id());
  const GroupChange leave = carol.Leave();
  const grs::ChangeReceived received = Receive(leave);
  EXPECT_EQ(received.held, grs::ChangeHeld::NoCard) << "kept; the caller asks for the card";
  EXPECT_TRUE(roster_->IsMember(kGroupId, carol.Id()).value()) << "it does not count yet";
  EXPECT_EQ(Held(), 1u);

  book.Hold(carol.Card()); // the card arrives
  auto applied = grs::ApplyView(*roster_, kGroupId, Cards(), kNowMs);
  ASSERT_TRUE(applied);
  EXPECT_FALSE(roster_->IsMember(kGroupId, carol.Id()).value());
  ASSERT_EQ(applied->newly_applied.size(), 1u);
  EXPECT_EQ(grc::Id(applied->newly_applied.front()), grc::Id(leave));
  EXPECT_EQ(grs::GroupsWithChangesBy(*roster_, carol.Id()), std::vector<std::string>{kGroupId});
}

TEST_F(GroupChangesWiredTest, ACarriedCardProvesTheChangeAndACopyWithoutItDoesNotReplaceIt) {
  Seed(*roster_, Statement());
  book.cards.erase(carol.Id());
  GroupChange leave = carol.Leave();
  leave.signer_card = carol.Card();
  EXPECT_EQ(Receive(leave).held, grs::ChangeHeld::Proved);
  EXPECT_FALSE(roster_->IsMember(kGroupId, carol.Id()).value());

  GroupChange bare = leave;
  bare.signer_card.reset();
  EXPECT_EQ(Receive(bare).held, grs::ChangeHeld::NoCard);
  const std::vector<GroupChange> held = grs::HeldChanges(*roster_, kGroupId);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_TRUE(held.front().signer_card) << "the copy that proves itself stays";
  EXPECT_FALSE(roster_->IsMember(kGroupId, carol.Id()).value());
}

TEST_F(GroupChangesWiredTest, AChangeFromSomeoneRemovedDoesNotCount) {
  Seed(*roster_, Statement([&](GroupRosterStatement& d) {
    d.members.erase(d.members.begin() + 2); // Carol out
    d.removed = {{.identity = carol.Id(), .removal = "r1", .removed_by = bob.Id()}};
  }));
  const grs::ChangeReceived received = Receive(carol.Add(erin.Id()));
  EXPECT_EQ(received.held, grs::ChangeHeld::Proved) << "it proves itself: held, so the owner folds it void";
  EXPECT_TRUE(received.applied.newly_applied.empty()) << "no line";
  EXPECT_FALSE(roster_->IsMember(kGroupId, erin.Id()).value());
  ASSERT_TRUE(received.applied.view);
  EXPECT_EQ(received.applied.view->Verdict(grc::Id(carol.Add(erin.Id())))->reason, GroupChangeReason::SignerOut);
}

TEST_F(GroupChangesWiredTest, AnAdminsRemovalCountsAndAMembersDoesNot) {
  Seed(*roster_, Statement());
  const grs::ChangeReceived by_member = Receive(dave.Remove(carol.Id()));
  EXPECT_TRUE(roster_->IsMember(kGroupId, carol.Id()).value()) << "a member removes no one (G017)";
  EXPECT_TRUE(by_member.applied.newly_applied.empty());

  const GroupChange by_admin = bob.Remove(carol.Id());
  const grs::ChangeReceived received = Receive(by_admin);
  EXPECT_FALSE(roster_->IsMember(kGroupId, carol.Id()).value());
  EXPECT_EQ(received.applied.gone, std::vector<std::string>{carol.Id()});
  ASSERT_TRUE(received.applied.view);
  ASSERT_EQ(received.applied.view->removed.size(), 1u);
  EXPECT_EQ(received.applied.view->removed[0].removed_by, bob.Id());
  EXPECT_EQ(received.applied.view->removed[0].removal, grc::Id(by_admin));
  EXPECT_TRUE(Receive(bob.Remove(alice.Id())).applied.gone.empty()) << "nor the owner";
}

TEST_F(GroupChangesWiredTest, AnOwnersLeaveDoesNotCount) {
  Seed(*roster_, Statement());
  GroupChange draft = alice.Draft(GroupChangeKind::Leave);
  const grs::ChangeReceived received = Receive(alice.Signed(draft));
  EXPECT_TRUE(roster_->IsMember(kGroupId, alice.Id()).value()) << "the owner transfers or dissolves (G009, G016)";
  EXPECT_TRUE(received.applied.newly_applied.empty());
}

// ---- The producers ----

TEST_F(GroupChangesWiredTest, ALeaveNamesTheAddThatBroughtUsInAndOurOwnChanges) {
  Seed(*roster_, Statement());
  const GroupChange brings_erin = carol.Add(erin.Id());
  Receive(brings_erin);
  const GroupChange erins_add = erin.Add("account:frank");
  Receive(erins_add);
  auto leave = grs::DraftLeave(*roster_, kGroupId, erin.Id(), erin.Device().peer_id, kNowMs);
  ASSERT_TRUE(leave) << leave.error().message;
  EXPECT_EQ(leave->kind, GroupChangeKind::Leave);
  EXPECT_EQ(leave->base_epoch, 5u) << "the statement it was made against";
  EXPECT_EQ(leave->adds, std::vector<std::string>{grc::Id(brings_erin)});
  EXPECT_EQ(leave->keeps, std::vector<std::string>{grc::Id(erins_add)}) << "Frank, whom Erin brought, stays (R3)";
  EXPECT_FALSE(grs::DraftLeave(*roster_, kGroupId, alice.Id(), alice.Device().peer_id, kNowMs)) << "not the owner";

  // Signed by Erin's device, it takes Erin out and keeps Frank.
  Receive(erin.Signed(*leave));
  EXPECT_FALSE(roster_->IsMember(kGroupId, erin.Id()).value());
  EXPECT_TRUE(roster_->IsMember(kGroupId, "account:frank").value());
}

TEST_F(GroupChangesWiredTest, ARemovalIsDraftedOnlyWhereTheRosterAllowsIt) {
  Seed(*roster_, Statement());
  const GroupChange brings_erin = carol.Add(erin.Id());
  Receive(brings_erin);
  const GroupChange erins_add = erin.Add("account:frank");
  Receive(erins_add);
  auto removal = grs::DraftRemove(*roster_, kGroupId, bob.Id(), bob.Device().peer_id, erin.Id(), kNowMs);
  ASSERT_TRUE(removal) << removal.error().message;
  EXPECT_EQ(removal->member, erin.Id());
  EXPECT_EQ(removal->adds, std::vector<std::string>{grc::Id(brings_erin)});
  EXPECT_EQ(removal->keeps, std::vector<std::string>{grc::Id(erins_add)});

  EXPECT_FALSE(grs::DraftRemove(*roster_, kGroupId, bob.Id(), bob.Device().peer_id, alice.Id(), kNowMs))
      << "an admin removes members only";
  EXPECT_FALSE(grs::DraftRemove(*roster_, kGroupId, dave.Id(), dave.Device().peer_id, carol.Id(), kNowMs))
      << "a member removes no one";
  EXPECT_TRUE(grs::DraftRemove(*roster_, kGroupId, alice.Id(), alice.Device().peer_id, bob.Id(), kNowMs))
      << "the owner removes an admin";
}

// ---- The owner's fold (G018 item 4) ----

TEST_F(GroupChangesWiredTest, TheOwnerFoldsWhatItHoldsAndMembersDropWhatWasFolded) {
  Seed(*roster_, Statement()); // the owner's device
  InMemoryDeliveryStore member_threads;
  InMemoryGroupRosterStore member(member_threads); // Dave's device
  Seed(member, Statement());
  const GroupChange removal = bob.Remove(carol.Id());
  const GroupChange brings_erin = dave.Add(erin.Id());
  for (IGroupRosterStore* device :
       {static_cast<IGroupRosterStore*>(roster_.get()), static_cast<IGroupRosterStore*>(&member)}) {
    Receive(removal, device);
    Receive(brings_erin, device);
  }
  EXPECT_TRUE(grs::HoldsFoldable(*roster_, kGroupId, Cards(), kNowMs));

  std::vector<GroupRosterStatementMember> rows;
  const std::vector<GroupRosterMember> owner_rows = roster_->ListMembers(kGroupId).value();
  for (const GroupRosterMember& m : owner_rows) {
    rows.push_back({m.member_identity, m.role, m.joined_at, m.added_by});
  }
  GroupRosterStatement draft =
      grs::OwnerDraft(*roster_, Metadata(), rows, {}, alice.Id(), alice.Key(), Cards(), kNowMs);
  EXPECT_EQ(draft.roster_epoch, 6u) << "it says something new: the next epoch";
  const GroupRosterStatement next = test::SignedStatement(alice, draft);
  ASSERT_TRUE(group_roster_statement::Verify(next, alice.Id()));
  EXPECT_TRUE(next.Folded(grc::Id(removal)));
  EXPECT_TRUE(next.Folded(grc::Id(brings_erin)));
  ASSERT_EQ(next.removed.size(), 1u);
  EXPECT_EQ(next.removed[0].identity, carol.Id());
  EXPECT_EQ(next.removed[0].removal, grc::Id(removal)) << "what a re-admit names";
  EXPECT_TRUE(next.Lists(erin.Id()));

  // The owner keeps it; so does Dave, who hears it: both drop the folded changes and agree.
  ASSERT_TRUE(*grs::KeepStatement(*roster_, next));
  ASSERT_TRUE(grs::ApplyView(*roster_, kGroupId, Cards(), kNowMs));
  ASSERT_TRUE(*grs::KeepAndCatchUp(member, next, Cards(), kNowMs));
  EXPECT_EQ(Held(), 0u);
  EXPECT_EQ(Held(&member), 0u);
  EXPECT_EQ(Members(), Members(&member));
  EXPECT_EQ(Members(), Sorted({alice.Id(), bob.Id(), dave.Id(), erin.Id()}));
  EXPECT_EQ(member.FindMember(kGroupId, erin.Id()).value()->added_by, dave.Id());
  EXPECT_FALSE(grs::HoldsFoldable(*roster_, kGroupId, Cards(), kNowMs));
  EXPECT_EQ(grs::CopyState(*roster_, kGroupId), grs::CopyState(member, kGroupId));
}

TEST_F(GroupChangesWiredTest, AnOwnerSigningNothingNewKeepsItsEpoch) {
  const GroupRosterStatement held = Statement();
  Seed(*roster_, held);
  std::vector<GroupRosterStatementMember> rows = held.members;
  const GroupRosterStatement same =
      grs::OwnerDraft(*roster_, Metadata(), rows, {}, alice.Id(), alice.Key(), Cards(), kNowMs);
  EXPECT_EQ(same.roster_epoch, held.roster_epoch);
  rows.push_back(Row(erin.Id()));
  const GroupRosterStatement grown =
      grs::OwnerDraft(*roster_, Metadata(), rows, {}, alice.Id(), alice.Key(), Cards(), kNowMs);
  EXPECT_EQ(grown.roster_epoch, held.roster_epoch + 1) << "never two statements at one epoch";
}

// ---- The change floor ----

TEST_F(GroupChangesWiredTest, AChangeBelowTheFloorIsDroppedAsTooOld) {
  Seed(*roster_, Statement([](GroupRosterStatement& d) {
    d.roster_epoch = 40;
    d.change_floor_epoch = 8;
  }));
  const grs::ChangeReceived received = Receive(carol.Add(erin.Id())); // made against epoch 5
  EXPECT_EQ(received.held, grs::ChangeHeld::Proved);
  EXPECT_EQ(Held(), 0u) << "lost: a device offline that long redoes it";
  EXPECT_FALSE(roster_->IsMember(kGroupId, erin.Id()).value());
}

// ---- Keeping members in step (G018 item 5) ----

TEST_F(GroupChangesWiredTest, ACopySaysWhichRosterItsSenderHolds) {
  Seed(*roster_, Statement());
  const GroupCopyState empty = grs::CopyState(*roster_, kGroupId);
  EXPECT_EQ(empty.statement_epoch, 5u);
  EXPECT_EQ(empty.changes_hash, grc::ChangesHash({}));
  const GroupChange add = carol.Add(erin.Id());
  Receive(add);
  const GroupCopyState held = grs::CopyState(*roster_, kGroupId);
  EXPECT_EQ(held.changes_hash, grc::ChangesHash({grc::Id(add)}));

  EXPECT_TRUE(grs::NeedsCatchUp(empty, held)) << "same statement, other changes";
  EXPECT_TRUE(grs::NeedsCatchUp(empty, {.statement_epoch = 6, .changes_hash = empty.changes_hash}));
  EXPECT_FALSE(grs::NeedsCatchUp(held, {.statement_epoch = 4, .changes_hash = "ff"})) << "a sender behind us asks us";
  EXPECT_FALSE(grs::NeedsCatchUp(held, held));
}

TEST_F(GroupChangesWiredTest, ABurstOfCopiesAsksOnce) {
  grs::CatchUpLimiter limiter;
  EXPECT_TRUE(limiter.Allow(kGroupId, 1'000));
  EXPECT_FALSE(limiter.Allow(kGroupId, 1'500));
  EXPECT_FALSE(limiter.Allow(kGroupId, 1'000 + grs::CatchUpLimiter::kCatchUpGapMs - 1));
  EXPECT_TRUE(limiter.Allow("group:other", 1'500)) << "per group";
  EXPECT_TRUE(limiter.Allow(kGroupId, 1'000 + grs::CatchUpLimiter::kCatchUpGapMs));
}

TEST_F(GroupChangesWiredTest, TheMemberCheckReplyCarriesTheChangesTheAskerLacks) {
  Seed(*roster_, Statement()); // the replier: Carol's device
  InMemoryDeliveryStore asker_threads;
  InMemoryGroupRosterStore asker(asker_threads); // Dave's device, which missed them
  Seed(asker, Statement());
  const GroupChange removal = bob.Remove(carol.Id());
  const GroupChange brings_erin = dave.Add(erin.Id());
  Receive(removal);
  Receive(brings_erin);

  auto request = grs::BeginCatchUp(asker, kGroupId, kNowMs);
  ASSERT_TRUE(request && *request);
  EXPECT_EQ((*request)->statement_epoch, 5u);
  EXPECT_EQ((*request)->changes_hash, grc::ChangesHash({}));
  EXPECT_EQ(Metadata(&asker).sync_state, GroupSyncState::Active) << "a catch-up does not stop sends";

  auto reply = grs::Answer(*roster_, **request, dave.Id());
  ASSERT_TRUE(reply && *reply);
  EXPECT_EQ((*reply)->changes.size(), 2u);
  EXPECT_FALSE((*reply)->more);
  // Over the wire and applied: the asker holds what the replier holds.
  auto decoded = group_roster_sync_codec::DecodeReply(group_roster_sync_codec::EncodeReply(**reply));
  ASSERT_TRUE(decoded) << decoded.error().message;
  grs::GroupViewApplied applied;
  auto outcome = grs::ApplyReply(asker, *decoded, dave.Id(), carol.Id(), Cards(), kNowMs, nullptr, &applied);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(*outcome, grs::ReplyOutcome::Active);
  EXPECT_EQ(Members(&asker), Members());
  EXPECT_EQ(grs::CopyState(asker, kGroupId), grs::CopyState(*roster_, kGroupId));
  EXPECT_EQ(applied.newly_applied.size(), 2u) << "each wants its line";

  // Asked again with the same changes: nothing to carry.
  auto even = grs::BeginCatchUp(asker, kGroupId, kNowMs);
  auto quiet = grs::Answer(*roster_, **even, dave.Id());
  EXPECT_TRUE((*quiet)->changes.empty());
}

TEST_F(GroupChangesWiredTest, AReplyCarriesABoundedPartAndTheAskerAsksForMore) {
  Seed(*roster_, Statement());
  InMemoryDeliveryStore asker_threads;
  InMemoryGroupRosterStore asker(asker_threads);
  Seed(asker, Statement());
  for (int i = 0; i < 30; ++i) {
    GroupChange add = carol.Add("account:guest-" + std::to_string(i), "invite-" + std::to_string(i));
    add.signer_card = carol.Card(); // each copy as first sent, with the card
    add = carol.Signed(add);
    Receive(add);
  }
  size_t asked = 0;
  std::string after;
  for (; asked < 10; ++asked) {
    auto request = grs::BeginCatchUp(asker, kGroupId, kNowMs, after);
    auto reply = grs::Answer(*roster_, **request, dave.Id());
    ASSERT_TRUE(reply && *reply);
    ASSERT_FALSE((*reply)->changes.empty());
    EXPECT_LE(group_roster_sync_codec::EncodeReply(**reply).size(),
              group_roster_sync_codec::kReplyChangesBudgetBytes + 32 * 1024)
        << "bounded";
    size_t carded = 0;
    for (const GroupChange& c : (*reply)->changes) {
      carded += c.signer_card ? 1 : 0;
    }
    EXPECT_EQ(carded, 1u) << "the signer's card rides once per reply";
    ASSERT_TRUE(grs::ApplyReply(asker, **reply, dave.Id(), carol.Id(), Cards(), kNowMs));
    if (!(*reply)->more) {
      break;
    }
    after = grc::Id((*reply)->changes.back());
  }
  EXPECT_GT(asked, 0u) << "30 changes with a card do not fit one reply";
  EXPECT_EQ(Held(&asker), 30u);
  EXPECT_EQ(Members(&asker), Members());
}

TEST_F(GroupChangesWiredTest, ACheckRunningIsNotOverriddenByACatchUp) {
  Seed(*roster_, Statement());
  ASSERT_TRUE(grs::BeginCheck(*roster_, kGroupId, dave.Id(), kNowMs));
  auto request = grs::BeginCatchUp(*roster_, kGroupId, kNowMs);
  ASSERT_TRUE(request);
  EXPECT_FALSE(*request) << "the check asks everyone, and carries our state too";
}

// ---- Lines ----

TEST_F(GroupChangesWiredTest, AppliedChangesReadAsTheGroupsMembershipLines) {
  auto thread = store_->FindOrCreateGroupThread(kGroupId, "Hikers", {});
  ASSERT_TRUE(thread);
  auto about_us = group_change_lines::Append(
      *store_, kGroupId, {carol.Leave(), bob.Remove(dave.Id()), carol.Rename("Bikers", 6)}, 7, dave.Id());
  ASSERT_TRUE(about_us);
  EXPECT_EQ(about_us->value_or(""), thread->id) << "Dave reads that he was removed";
  const std::vector<ThreadMessage> lines = store_->GetMessages(thread->id).value();
  ASSERT_EQ(lines.size(), 3u) << "a leave, a removal and a rename each read as a line";
  auto left = GroupMembershipCodec::DecodeMemberLeftFromMessage(lines[0]);
  ASSERT_TRUE(left);
  EXPECT_EQ(left->member_identity, carol.Id());
  auto removed = GroupMembershipCodec::DecodeMemberRemovedFromMessage(lines[1]);
  ASSERT_TRUE(removed);
  EXPECT_EQ(removed->member_identity, dave.Id());
  EXPECT_EQ(removed->removed_by, bob.Id());
  EXPECT_EQ(lines[1].sender_contact_id, bob.Id());
  // G017: a member's rename reads as "Carol renamed the group …", from its signer.
  auto renamed = GroupMembershipCodec::DecodeGroupRenamedFromMessage(lines[2]);
  ASSERT_TRUE(renamed);
  EXPECT_EQ(renamed->title, "Bikers");
  EXPECT_EQ(renamed->title_version, 6u);
  EXPECT_EQ(lines[2].sender_contact_id, carol.Id());
}

} // namespace
} // namespace pbr
