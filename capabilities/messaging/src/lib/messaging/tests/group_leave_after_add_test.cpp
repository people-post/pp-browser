// G018: a member brought in by an add leaves, or is removed. What every other member's roster (the rows the member
// list shows) says once the leave or removal arrives — the adder's, a third member's and the owner's — when the add
// was still held (unfolded), when the owner had folded it, and when the leaver (remover) held the statement that
// folded it but the receiver did not. Device-level, as the receive path runs it: ReceiveChange on each device's own
// store, the leave (removal) drafted by its signer's device from what it holds (DraftLeave, DraftRemove).
#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/tests/GroupChangeTestKit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
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

/** One device: its threads and its roster store. */
struct Device {
  InMemoryDeliveryStore threads;
  std::unique_ptr<InMemoryGroupRosterStore> roster;

  Device() {
    EXPECT_TRUE(threads.ListThreads());
    roster = std::make_unique<InMemoryGroupRosterStore>(threads);
  }
};

/**
 * Alice owns, Bob is admin, Carol and Dave are members (statement epoch 5). Erin (M) is brought in by an add and
 * leaves. Each person has their own device.
 */
class GroupLeaveAfterAddTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (Person* p : {&alice, &bob, &carol, &dave, &erin}) {
      book.Hold(p->Card());
    }
  }

  GroupRosterStatement S5() {
    GroupRosterStatement draft;
    draft.roster_epoch = 5;
    draft.members = {Row(alice.Id(), MemberRole::Owner), Row(bob.Id(), MemberRole::Admin), Row(carol.Id()),
                     Row(dave.Id())};
    draft.title = "Hikers";
    draft.title_version = 1;
    return test::SignedStatement(alice, draft);
  }

  GroupCardLookup Cards() const { return book.Lookup(); }

  /** The device holds `statement`, as a roster event left it. */
  void Seed(Device& d, const GroupRosterStatement& statement) {
    GroupMetadata metadata;
    metadata.group_id = kGroupId;
    metadata.owner_identity = alice.Id();
    metadata.roster_epoch = statement.roster_epoch;
    ASSERT_TRUE(d.roster->UpsertMetadata(metadata));
    ASSERT_TRUE(*grs::KeepStatement(*d.roster, statement));
    ASSERT_TRUE(grs::ApplyView(*d.roster, kGroupId, Cards(), kNowMs));
  }

  /** The newcomer's device takes the join bundle (RelayReceivePipeline::ApplyJoinBundle): statement + carried. */
  void Join(Device& d, const GroupRosterStatement& statement, const std::vector<GroupChange>& carried) {
    GroupMetadata metadata;
    metadata.group_id = kGroupId;
    metadata.owner_identity = alice.Id();
    metadata.roster_epoch = statement.roster_epoch;
    ASSERT_TRUE(d.roster->UpsertMetadata(metadata));
    ASSERT_TRUE(grs::CheckJoin(*d.roster, statement, carried, Cards(), kNowMs, erin.Id()));
    ASSERT_TRUE(*grs::KeepStatement(*d.roster, statement));
    ASSERT_TRUE(grs::HoldCarried(*d.roster, carried, Cards(), kNowMs, /*line_shown=*/true));
    auto applied = grs::ApplyView(*d.roster, kGroupId, Cards(), kNowMs);
    ASSERT_TRUE(applied && applied->view && applied->view->Lists(erin.Id()));
  }

  grs::ChangeReceived Receive(Device& d, const GroupChange& change) {
    auto received = grs::ReceiveChange(*d.roster, change, Cards(), kNowMs);
    EXPECT_TRUE(received) << received.error().message;
    return received ? *received : grs::ChangeReceived{};
  }

  /** Erin's device drafts and signs its leave from what it holds (GroupMembershipWorkflow::LeaveGroup). */
  GroupChange ErinLeaves(Device& d) {
    auto draft = grs::DraftLeave(*d.roster, kGroupId, erin.Id(), erin.Device().peer_id, kNowMs);
    EXPECT_TRUE(draft) << draft.error().message;
    return erin.Signed(*draft);
  }

  /** The owner's fold of what its device holds, signed (GroupMembershipWorkflow::SignRoster), kept there. */
  GroupRosterStatement OwnerFolds(Device& owner) {
    std::vector<GroupRosterStatementMember> rows;
    const std::vector<GroupRosterMember> members = owner.roster->ListMembers(kGroupId).value();
    for (const GroupRosterMember& m : members) {
      rows.push_back({m.member_identity, m.role, m.joined_at, m.added_by});
    }
    const GroupMetadata metadata = owner.roster->LoadMetadata(kGroupId).value().value();
    GroupRosterStatement draft =
        grs::OwnerDraft(*owner.roster, metadata, rows, {}, alice.Id(), alice.Key(), Cards(), kNowMs);
    auto signed_ = group_roster_statement::SignStatement(alice, alice.Key(), std::move(draft));
    EXPECT_TRUE(signed_) << signed_.error().message;
    const GroupRosterStatement next = signed_ ? *signed_ : GroupRosterStatement{};
    auto kept = grs::KeepAndCatchUp(*owner.roster, next, Cards(), kNowMs);
    EXPECT_TRUE(kept && *kept) << (kept ? "not newer" : kept.error().message);
    return next;
  }

  /** What the member list shows: the device's roster rows. */
  bool Lists(Device& d, const Person& p) { return d.roster->IsMember(kGroupId, p.Id()).value(); }

  static const GroupChangeVerdict* Verdict(const grs::ChangeReceived& r, const GroupChange& c) {
    return r.applied.view ? r.applied.view->Verdict(grc::Id(c)) : nullptr;
  }

  Person& alice = Named("alice");
  Person& bob = Named("bob");
  Person& carol = Named("carol");
  Person& dave = Named("dave");
  Person& erin = Named("erin");
  test::CardBook book;
};

// (a) Carol (not the owner) adds Erin; the owner is offline, so the add stays held everywhere. Erin's join bundle
// carried the add, so her leave names it: on the adder's and a third member's device the leave undoes the add (R1).
TEST_F(GroupLeaveAfterAddTest, AHeldMembersAddThenTheLeaveOwnerOfflineEveryMemberDropsTheLeaver) {
  Device carol_d, dave_d, erin_d;
  Seed(carol_d, S5());
  Seed(dave_d, S5());
  const GroupChange add = carol.Add(erin.Id());
  Receive(carol_d, add); // AdmitNewcomer holds its own add
  Receive(dave_d, add);  // FanOutChange
  Join(erin_d, S5(), {add});
  ASSERT_TRUE(Lists(carol_d, erin));
  ASSERT_TRUE(Lists(dave_d, erin));

  const GroupChange leave = ErinLeaves(erin_d);
  EXPECT_EQ(leave.adds, std::vector<std::string>{grc::Id(add)}) << "the add the join bundle carried";

  for (Device* d : {&carol_d, &dave_d}) {
    const grs::ChangeReceived r = Receive(*d, leave);
    EXPECT_FALSE(Lists(*d, erin)) << "gone as soon as the leave arrives";
    EXPECT_EQ(r.applied.gone, std::vector<std::string>{erin.Id()});
    ASSERT_TRUE(Verdict(r, leave));
    EXPECT_EQ(Verdict(r, leave)->status, GroupChangeStatus::Applied);
    ASSERT_TRUE(Verdict(r, add));
    EXPECT_EQ(Verdict(r, add)->status, GroupChangeStatus::Undone);
    EXPECT_EQ(Verdict(r, add)->reason, GroupChangeReason::NamedByLeave);
  }
}

// (b) The same, the receiver is the owner: Erin goes at once and the owner has something to fold.
TEST_F(GroupLeaveAfterAddTest, AHeldMembersAddThenTheLeaveTheOwnerDropsTheLeaverAndFolds) {
  Device alice_d, erin_d;
  Seed(alice_d, S5());
  const GroupChange add = carol.Add(erin.Id());
  Receive(alice_d, add);
  Join(erin_d, S5(), {add});
  const GroupChange leave = ErinLeaves(erin_d);

  Receive(alice_d, leave);
  EXPECT_FALSE(Lists(alice_d, erin));
  EXPECT_TRUE(grs::HoldsFoldable(*alice_d.roster, kGroupId, Cards(), kNowMs));
  const GroupRosterStatement folded = OwnerFolds(alice_d);
  EXPECT_FALSE(folded.Lists(erin.Id()));
  EXPECT_TRUE(folded.Folded(grc::Id(add)));
  EXPECT_TRUE(folded.Folded(grc::Id(leave)));
}

// (c) The owner adds Erin and folds the add (statement 6 lists her); every member holds statement 6. Erin's leave
// names no add (hers was folded), and counts against the statement on a member's device.
TEST_F(GroupLeaveAfterAddTest, AnOwnersFoldedAddThenTheLeaveAMemberDropsTheLeaver) {
  Device alice_d, carol_d, erin_d;
  Seed(alice_d, S5());
  Seed(carol_d, S5());
  const GroupChange add = alice.Add(erin.Id());
  Receive(alice_d, add);
  Receive(carol_d, add);
  Join(erin_d, S5(), {add}); // the bundle goes before the fold
  const GroupRosterStatement s6 = OwnerFolds(alice_d);
  ASSERT_TRUE(s6.Lists(erin.Id()));
  ASSERT_TRUE(*grs::KeepAndCatchUp(*carol_d.roster, s6, Cards(), kNowMs));
  ASSERT_TRUE(*grs::KeepAndCatchUp(*erin_d.roster, s6, Cards(), kNowMs));

  const GroupChange leave = ErinLeaves(erin_d); // names statement 6's folded ids (Erin's add among them)
  const grs::ChangeReceived r = Receive(carol_d, leave);
  EXPECT_FALSE(Lists(carol_d, erin));
  ASSERT_TRUE(Verdict(r, leave));
  EXPECT_EQ(Verdict(r, leave)->status, GroupChangeStatus::Applied);
}

// (d) The add was folded into statement 6, which reached Erin but not Carol (the owner's SignAndSendRoster is
// best-effort; here the owner then went offline). Carol still holds the add unfolded and no statement listing Erin.
// Erin's device dropped the add as folded, so before the fix her leave named no add: R1 kept the add on Carol's
// device and Erin stayed listed (with the leave "applied") until a newer statement reached Carol. Now the leave
// names the ids statement 6 folded, Erin's add among them.
TEST_F(GroupLeaveAfterAddTest, TheLeaverHeldANewerStatementThanTheReceiverAndTheReceiverDropsThemAllTheSame) {
  for (const bool owners_add : {false, true}) {
    SCOPED_TRACE(owners_add ? "the owner's add" : "Carol's add");
    Device alice_d, carol_d, erin_d;
    Seed(alice_d, S5());
    Seed(carol_d, S5());
    const GroupChange add = owners_add ? alice.Add(erin.Id()) : carol.Add(erin.Id());
    Receive(alice_d, add);
    Receive(carol_d, add);
    Join(erin_d, S5(), {add});
    const GroupRosterStatement s6 = OwnerFolds(alice_d);
    ASSERT_TRUE(*grs::KeepAndCatchUp(*erin_d.roster, s6, Cards(), kNowMs)); // Carol misses it

    const GroupChange leave = ErinLeaves(erin_d);
    ASSERT_GT(leave.base_epoch, 5u) << "made against a statement Carol does not hold";
    EXPECT_TRUE(std::find(leave.adds.begin(), leave.adds.end(), grc::Id(add)) != leave.adds.end())
        << "the folded add, from statement 6's folded list";
    const grs::ChangeReceived r = Receive(carol_d, leave);
    ASSERT_TRUE(Verdict(r, leave));
    EXPECT_EQ(Verdict(r, leave)->status, GroupChangeStatus::Applied);
    EXPECT_FALSE(Lists(carol_d, erin)) << "a leave that counts takes the leaver off the list";
    ASSERT_TRUE(Verdict(r, add));
    EXPECT_EQ(Verdict(r, add)->status, GroupChangeStatus::Undone);
    EXPECT_EQ(Verdict(r, add)->reason, GroupChangeReason::NamedByLeave);
  }
}

// A newer statement reaching Carol settles it too: the owner, online again, folds the leave (statement 7).
TEST_F(GroupLeaveAfterAddTest, ALaterStatementFromTheOwnerClearsAStaleListing) {
  Device alice_d, carol_d, erin_d;
  Seed(alice_d, S5());
  Seed(carol_d, S5());
  const GroupChange add = carol.Add(erin.Id());
  Receive(alice_d, add);
  Receive(carol_d, add);
  Join(erin_d, S5(), {add});
  const GroupRosterStatement s6 = OwnerFolds(alice_d);
  ASSERT_TRUE(*grs::KeepAndCatchUp(*erin_d.roster, s6, Cards(), kNowMs));
  const GroupChange leave = ErinLeaves(erin_d);
  Receive(carol_d, leave);

  Receive(alice_d, leave);
  const GroupRosterStatement s7 = OwnerFolds(alice_d);
  ASSERT_FALSE(s7.Lists(erin.Id()));
  ASSERT_TRUE(*grs::KeepAndCatchUp(*carol_d.roster, s7, Cards(), kNowMs));
  EXPECT_FALSE(Lists(carol_d, erin));
}

// A receiver whose code predates the fix: the fix is in the drafts only (DraftLeave, DraftRemove), so the receive path
// below (ReceiveChange, the roster rules) is the one older apps run. The leave is built by hand with the list a fixed
// leaver sends (statement 6's folded ids), not drafted, so nothing of the new draft code is involved on this side.
TEST_F(GroupLeaveAfterAddTest, AReceiverOnTheOldReceivePathDropsTheLeaverFromAFixedLeaversLongerList) {
  Device alice_d, carol_d;
  Seed(alice_d, S5());
  Seed(carol_d, S5());
  const GroupChange add = carol.Add(erin.Id());
  const GroupChange franks = dave.Add("account:frank", "invite-2");
  for (Device* d : {&alice_d, &carol_d}) {
    Receive(*d, add);
    Receive(*d, franks);
  }
  const GroupRosterStatement s6 = OwnerFolds(alice_d); // Carol misses it
  std::vector<std::string> named;
  for (const GroupFoldedChange& f : s6.folded) {
    named.push_back(f.id);
  }
  ASSERT_EQ(named.size(), 2u);
  const GroupChange leave = erin.Leave(named);
  const grs::ChangeReceived r = Receive(carol_d, leave);
  EXPECT_FALSE(Lists(carol_d, erin));
  EXPECT_TRUE(carol_d.roster->IsMember(kGroupId, "account:frank").value())
      << "Frank's add, named too, is not Erin's: it stands";
  ASSERT_TRUE(Verdict(r, franks));
  EXPECT_EQ(Verdict(r, franks)->status, GroupChangeStatus::Applied);
}

// A receiver that holds statement 6: the folded ids the leave names are dropped as folded there, and an id of
// another member's add it still holds changes nothing. Only Erin goes.
TEST_F(GroupLeaveAfterAddTest, TheLongerListIsHarmlessToAReceiverHoldingTheFoldingStatement) {
  Device alice_d, carol_d, erin_d;
  Seed(alice_d, S5());
  Seed(carol_d, S5());
  const GroupChange add = carol.Add(erin.Id());
  const GroupChange franks = dave.Add("account:frank", "invite-2");
  for (Device* d : {&alice_d, &carol_d}) {
    Receive(*d, add);
    Receive(*d, franks);
  }
  Join(erin_d, S5(), {add, franks});
  const GroupRosterStatement s6 = OwnerFolds(alice_d);
  ASSERT_TRUE(*grs::KeepAndCatchUp(*carol_d.roster, s6, Cards(), kNowMs));
  ASSERT_TRUE(*grs::KeepAndCatchUp(*erin_d.roster, s6, Cards(), kNowMs));
  const GroupChange zeds = bob.Add("account:zed", "invite-3"); // not folded by statement 6: held on both
  Receive(carol_d, zeds);
  Receive(erin_d, zeds);

  GroupChange draft = grs::DraftLeave(*erin_d.roster, kGroupId, erin.Id(), erin.Device().peer_id, kNowMs).value();
  draft.adds.push_back(grc::Id(zeds)); // and, by hand, an add that is not Erin's and that Carol holds unfolded
  std::sort(draft.adds.begin(), draft.adds.end());
  ASSERT_EQ(draft.adds.size(), 3u) << "statement 6's two folded ids and Zed's add";
  const GroupChange leave = erin.Signed(draft);
  const grs::ChangeReceived r = Receive(carol_d, leave);
  EXPECT_EQ(r.applied.gone, std::vector<std::string>{erin.Id()});
  for (const std::string& still :
       {alice.Id(), bob.Id(), carol.Id(), dave.Id(), std::string("account:frank"), std::string("account:zed")}) {
    EXPECT_TRUE(carol_d.roster->IsMember(kGroupId, still).value()) << still;
  }
  ASSERT_TRUE(Verdict(r, zeds));
  EXPECT_EQ(Verdict(r, zeds)->status, GroupChangeStatus::Applied);
  EXPECT_EQ(Verdict(r, leave)->status, GroupChangeStatus::Applied);
}

/** A change id that names nothing (64 hex digits). */
std::string FakeId(const int i) {
  char buf[65];
  std::snprintf(buf, sizeof buf, "%064x", i);
  return buf;
}

// The bound: a change names at most kMaxNamedChanges ids. The leaver's own held adds go first, then the folded ids
// of the newest folds (highest base epoch); the oldest folds are left out.
TEST_F(GroupLeaveAfterAddTest, ALeaveNamesItsOwnHeldAddsFirstThenTheNewestFoldsUpToTheBound) {
  GroupRosterStatement draft = S5();
  draft.roster_epoch = 400;
  draft.owner_signature_b64.clear();
  for (int i = 0; i < 300; ++i) {
    draft.folded.push_back({.id = FakeId(i), .base_epoch = static_cast<uint64_t>(i)});
  }
  const GroupRosterStatement big = test::SignedStatement(alice, draft);
  const GroupChange add = carol.Add(erin.Id()); // still held: made after those folds
  Device erin_d;
  Join(erin_d, big, {add});

  const GroupChange leave = ErinLeaves(erin_d);
  ASSERT_EQ(leave.adds.size(), grc::kMaxNamedChanges);
  EXPECT_TRUE(std::is_sorted(leave.adds.begin(), leave.adds.end()));
  const auto names = [&](const std::string& id) {
    return std::find(leave.adds.begin(), leave.adds.end(), id) != leave.adds.end();
  };
  EXPECT_TRUE(names(grc::Id(add))) << "its own held add first";
  const int first_kept = 300 - static_cast<int>(grc::kMaxNamedChanges - 1); // 255 folded ids fit beside it
  for (int i = 0; i < 300; ++i) {
    EXPECT_EQ(names(FakeId(i)), i >= first_kept) << "folded at base epoch " << i;
  }
  EXPECT_TRUE(grc::CheckShape(leave)) << "within the bound a receiver checks";
}

// ---- Removal by an admin (G020): the same gap and the same fix ----

// Alice (the owner) adds Erin and folds it into statement 6, which reaches Bob (admin) but not Carol. Bob removes
// Erin. R2 voids a *member's* add the removal did not see, but an owner's or admin's add stands unless the removal
// names it: before the fix Bob's removal named no add (his device dropped it as folded) and Carol kept Erin listed.
TEST_F(GroupLeaveAfterAddTest, AnAdminRemovesSomeoneWhoseOwnersAddTheReceiverHoldsUnfolded) {
  for (const bool admins_add : {false, true}) {
    SCOPED_TRACE(admins_add ? "Bob's own add" : "the owner's add");
    Device alice_d, bob_d, carol_d;
    Seed(alice_d, S5());
    Seed(bob_d, S5());
    Seed(carol_d, S5());
    const GroupChange add = admins_add ? bob.Add(erin.Id()) : alice.Add(erin.Id());
    for (Device* d : {&alice_d, &bob_d, &carol_d}) {
      Receive(*d, add);
    }
    const GroupRosterStatement s6 = OwnerFolds(alice_d);
    ASSERT_TRUE(*grs::KeepAndCatchUp(*bob_d.roster, s6, Cards(), kNowMs)); // Carol misses it

    auto draft = grs::DraftRemove(*bob_d.roster, kGroupId, bob.Id(), bob.Device().peer_id, erin.Id(), kNowMs);
    ASSERT_TRUE(draft) << draft.error().message;
    EXPECT_TRUE(std::find(draft->adds.begin(), draft->adds.end(), grc::Id(add)) != draft->adds.end())
        << "the folded add, from statement 6's folded list";
    const GroupChange removal = bob.Signed(*draft);
    const grs::ChangeReceived r = Receive(carol_d, removal);
    EXPECT_FALSE(Lists(carol_d, erin)) << "removed: off the list";
    ASSERT_TRUE(Verdict(r, add));
    EXPECT_EQ(Verdict(r, add)->status, GroupChangeStatus::Undone);
    EXPECT_EQ(Verdict(r, add)->reason, GroupChangeReason::NamedByRemove);
    ASSERT_TRUE(r.applied.view);
    ASSERT_EQ(r.applied.view->removed.size(), 1u);
    EXPECT_EQ(r.applied.view->removed[0].identity, erin.Id());
  }
}

// ---- The receive path's verdict log line ----

TEST_F(GroupLeaveAfterAddTest, TheVerdictLineSaysWhetherAChangeCountedAndWhoItLeavesListed) {
  Device carol_d;
  Seed(carol_d, S5());
  const GroupChange add = carol.Add(erin.Id());
  grs::ChangeReceived r = Receive(carol_d, add);
  EXPECT_EQ(grs::VerdictLine(add, r.applied.view),
            std::string("kind=add group_id=") + kGroupId +
                " status=applied reason=none statement_epoch=5 base_epoch=5 names_adds=0 listed=1");

  const GroupChange leave = erin.Leave({grc::Id(add)});
  r = Receive(carol_d, leave);
  EXPECT_EQ(grs::VerdictLine(leave, r.applied.view),
            std::string("kind=leave group_id=") + kGroupId +
                " status=applied reason=none statement_epoch=5 base_epoch=5 names_adds=1 listed=0");

  const GroupChange stranger = Named("frank").Leave();
  book.Hold(Named("frank").Card());
  r = Receive(carol_d, stranger);
  EXPECT_NE(grs::VerdictLine(stranger, r.applied.view).find("status=void reason=signer_not_joined"), std::string::npos);

  EXPECT_EQ(grs::VerdictLine(add, std::nullopt),
            std::string("kind=add group_id=") + kGroupId + " view=none (no statement, or one older than the roster's)");
}

} // namespace
} // namespace pbr
