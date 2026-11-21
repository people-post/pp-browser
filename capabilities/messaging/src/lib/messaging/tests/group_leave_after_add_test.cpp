// G018: a member brought in by an add leaves. What every other member's roster (the rows the member list shows)
// says once the leave arrives — the adder's, a third member's and the owner's — when the add was still held
// (unfolded) and when the owner had folded it. Device-level, as the receive path runs it: ReceiveChange on each
// device's own store, the leave drafted by the leaver's device from what it holds (DraftLeave).
#include "lib/messaging/GroupRosterSync.h"
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

  const GroupChange leave = ErinLeaves(erin_d); // today it names no add: Erin's was dropped as folded
  const grs::ChangeReceived r = Receive(carol_d, leave);
  EXPECT_FALSE(Lists(carol_d, erin));
  ASSERT_TRUE(Verdict(r, leave));
  EXPECT_EQ(Verdict(r, leave)->status, GroupChangeStatus::Applied);
}

// (d) THE BUG (disabled until fixed). The add was folded into statement 6, which reached Erin but not Carol (the
// owner's SignAndSendRoster is best-effort; here the owner then went offline). Erin's leave names no add (hers was
// dropped as folded); Carol still holds the add unfolded and no statement listing Erin. Today on Carol's device:
// the add stays Applied (the leave does not name it), the leave is Applied too (Erin is "joining"), and Erin stays
// listed — Carol is not the owner, so nothing folds, and it stays so until a newer statement reaches her.
// Run with --gtest_also_run_disabled_tests to see it fail.
TEST_F(GroupLeaveAfterAddTest, DISABLED_TheLeaverHeldANewerStatementThanTheReceiverAndTheReceiverStillListsThem) {
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

    const GroupChange leave = ErinLeaves(erin_d); // today it names no add: Erin's was dropped as folded
    ASSERT_GT(leave.base_epoch, 5u) << "made against a statement Carol does not hold";
    const grs::ChangeReceived r = Receive(carol_d, leave);
    ASSERT_TRUE(Verdict(r, leave));
    EXPECT_EQ(Verdict(r, leave)->status, GroupChangeStatus::Applied);
    EXPECT_FALSE(Lists(carol_d, erin)) << "a leave that counts takes the leaver off the list";
  }
}

// What ends (d) today: a newer statement reaching Carol. The owner, online again, folds the leave (statement 7).
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
  Receive(carol_d, leave); // (d): Carol still lists Erin here today

  Receive(alice_d, leave);
  const GroupRosterStatement s7 = OwnerFolds(alice_d);
  ASSERT_FALSE(s7.Lists(erin.Id()));
  ASSERT_TRUE(*grs::KeepAndCatchUp(*carol_d.roster, s7, Cards(), kNowMs));
  EXPECT_FALSE(Lists(carol_d, erin));
}

} // namespace
} // namespace pbr
