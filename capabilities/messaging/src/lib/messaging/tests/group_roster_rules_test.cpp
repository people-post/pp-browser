// The roster from the owner's statement plus changes (G018 R1–R6), the owner's fold (item 4), and the core
// property: the same statement and the same set of changes give the same roster in any order.
#include "lib/messaging/GroupRosterRules.h"
#include "lib/messaging/tests/GroupChangeTestKit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <numeric>
#include <random>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using test::kNowMs;
using test::Named;
using test::Person;
using test::Row;
using Kind = GroupChangeKind;
using Reason = GroupChangeReason;
using Status = GroupChangeStatus;

namespace grc = group_change;

/** Members, roles, who brought whom, removed, links and title: what a roster is, without where it came from. */
std::string RosterOf(const GroupRosterView& view) {
  std::ostringstream out;
  out << "title " << view.title << "@" << view.title_version << "\n";
  for (const GroupRosterViewMember& m : view.members) {
    out << m.identity << " " << MemberRoleToString(m.role) << " by " << m.added_by << " at " << m.joined_at << "\n";
  }
  for (const GroupRemovedMember& r : view.removed) {
    out << "removed " << r.identity << " " << r.removal << " by " << r.removed_by << "\n";
  }
  for (const GroupInviteLink& l : view.links) {
    out << "link " << l.link_id << " " << l.maker << "\n";
  }
  return out.str();
}

class GroupRosterRulesTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (Person* p : {&alice, &bob, &carol, &dave, &erin, &frank}) {
      book.Hold(p->Card());
    }
  }

  /** Alice owns, Bob is admin, Carol and Dave are members; `edit` adjusts the draft before Alice signs. */
  GroupRosterStatement Statement(const std::function<void(GroupRosterStatement&)>& edit = {}) {
    GroupRosterStatement draft;
    draft.roster_epoch = 5;
    draft.members = {Row(alice.Id(), MemberRole::Owner), Row(bob.Id(), MemberRole::Admin), Row(carol.Id()),
                     Row(dave.Id())};
    draft.title = "Hikers";
    draft.title_version = 5;
    if (edit) {
      edit(draft);
    }
    return test::SignedStatement(alice, draft);
  }

  GroupRosterView Compute(const GroupRosterStatement& s, const std::vector<GroupChange>& changes) {
    return ComputeRoster(s, changes, book.Lookup(), kNowMs);
  }
  GroupRosterView Compute(const std::vector<GroupChange>& changes) { return Compute(Statement(), changes); }

  static std::vector<std::string> Ids(const GroupRosterView& view) {
    std::vector<std::string> ids;
    for (const GroupRosterViewMember& m : view.members) {
      ids.push_back(m.identity);
    }
    return ids;
  }

  std::vector<std::string> Expect(std::vector<std::string> ids) const {
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  static ::testing::AssertionResult Is(const GroupRosterView& view, const GroupChange& change, const Status status,
                                       const Reason reason = Reason::None) {
    const GroupChangeVerdict* v = view.Verdict(grc::Id(change));
    if (!v) {
      return ::testing::AssertionFailure() << "no verdict";
    }
    if (v->status != status || v->reason != reason) {
      return ::testing::AssertionFailure()
             << "status " << static_cast<int>(v->status) << " reason " << static_cast<int>(v->reason);
    }
    return ::testing::AssertionSuccess();
  }

  Person& alice = Named("alice");
  Person& bob = Named("bob");
  Person& carol = Named("carol");
  Person& dave = Named("dave");
  Person& erin = Named("erin");
  Person& frank = Named("frank");
  test::CardBook book;
};

// ---- R1: add and leave combine ----

TEST_F(GroupRosterRulesTest, AMembersAddBringsSomeoneInAndSaysWho) {
  const GroupChange add = carol.Add(erin.Id());
  const GroupRosterView view = Compute({add});
  EXPECT_EQ(Ids(view), Expect({alice.Id(), bob.Id(), carol.Id(), dave.Id(), erin.Id()}));
  const GroupRosterViewMember* e = view.Find(erin.Id());
  ASSERT_TRUE(e);
  EXPECT_EQ(e->role, MemberRole::Member);
  EXPECT_EQ(e->added_by, carol.Id());
  EXPECT_EQ(e->added_in, grc::Id(add));
  EXPECT_EQ(e->joined_at, add.at);
  EXPECT_TRUE(Is(view, add, Status::Applied));
  EXPECT_TRUE(view.Find(bob.Id())->added_in.empty()) << "the statement's members came from no change";
}

TEST_F(GroupRosterRulesTest, ALeaveThatNamesTheAddUndoesItAnAddItDidNotSeeStands) {
  const GroupChange add = carol.Add(erin.Id());
  const GroupChange leave = erin.Leave({grc::Id(add)});
  GroupRosterView view = Compute({add, leave});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(Is(view, add, Status::Undone, Reason::NamedByLeave));
  EXPECT_TRUE(Is(view, leave, Status::Applied));

  const GroupChange again = dave.Add(erin.Id(), "invite-2");
  view = Compute({add, leave, again});
  ASSERT_TRUE(view.Lists(erin.Id())) << "Dave brought Erin back after (or without seeing) her leave";
  EXPECT_EQ(view.Find(erin.Id())->added_by, dave.Id());
}

TEST_F(GroupRosterRulesTest, AStatementMemberLeavesButTheOwnerCannot) {
  const GroupChange dave_leaves = dave.Leave();
  const GroupChange alice_leaves = alice.Leave();
  const GroupRosterView view = Compute({dave_leaves, alice_leaves});
  EXPECT_FALSE(view.Lists(dave.Id()));
  EXPECT_TRUE(view.Lists(alice.Id()));
  EXPECT_TRUE(Is(view, dave_leaves, Status::Applied));
  EXPECT_TRUE(Is(view, alice_leaves, Status::Void, Reason::NotAllowed)) << "the owner transfers or dissolves (G009)";
}

TEST_F(GroupRosterRulesTest, ALeaveBySomeoneNobodyAddedWaitsForTheirAdd) {
  const GroupChange leave = frank.Leave();
  const GroupRosterView view = Compute({leave});
  EXPECT_TRUE(Is(view, leave, Status::Void, Reason::SignerNotJoined));
  EXPECT_FALSE(FoldsIn(*view.Verdict(grc::Id(leave)))) << "it may matter once Frank's add arrives";
}

// ---- R2: removal wins ----

TEST_F(GroupRosterRulesTest, ARemovalWinsOverAConcurrentMembersAdd) {
  const GroupChange seen = carol.Add(erin.Id());
  const GroupChange removal = bob.Remove(erin.Id(), {grc::Id(seen)});
  const GroupChange unseen = dave.Add(erin.Id(), "invite-2");
  const GroupRosterView view = Compute({seen, removal, unseen});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(Is(view, seen, Status::Undone, Reason::NamedByRemove));
  EXPECT_TRUE(Is(view, unseen, Status::Void, Reason::RemovedMember)) << "R2: voids a member's add it did not see";
  EXPECT_TRUE(Is(view, removal, Status::Applied));
  ASSERT_EQ(view.removed.size(), 1u);
  EXPECT_EQ(view.removed[0].identity, erin.Id());
  EXPECT_EQ(view.removed[0].removal, grc::Id(removal));
  EXPECT_EQ(view.removed[0].removed_by, bob.Id());
}

// G018 R2 voids "any *member's* add" the removal did not see: an owner's or admin's add it did not see stands.
TEST_F(GroupRosterRulesTest, AnOwnersAddTheRemovalDidNotSeeStands) {
  const GroupChange seen = carol.Add(erin.Id());
  const GroupChange removal = bob.Remove(erin.Id(), {grc::Id(seen)});
  const GroupChange owners = alice.Add(erin.Id(), "invite-3");
  const GroupRosterView view = Compute({seen, removal, owners});
  ASSERT_TRUE(view.Lists(erin.Id()));
  EXPECT_EQ(view.Find(erin.Id())->added_by, alice.Id());
  EXPECT_TRUE(view.removed.empty()) << "back in: not on the removed list";
}

TEST_F(GroupRosterRulesTest, AfterAFoldedRemovalOnlyAnOwnersOrAdminsReadmitNamingItBringsThemBack) {
  const GroupRosterStatement s = Statement([&](GroupRosterStatement& d) {
    d.removed = {{.identity = erin.Id(), .removal = "removal-1", .removed_by = bob.Id()}};
  });
  const GroupChange readmit = bob.Add(erin.Id(), "invite-1", "removal-1");
  GroupRosterView view = Compute(s, {readmit});
  ASSERT_TRUE(view.Lists(erin.Id()));
  EXPECT_TRUE(view.removed.empty());
  EXPECT_TRUE(Is(view, readmit, Status::Applied));

  const std::vector<std::pair<GroupChange, Reason>> refused = {
      {carol.Add(erin.Id(), "invite-1", "removal-1"), Reason::NotAllowed},  // a member cannot re-admit
      {carol.Add(erin.Id()), Reason::NotReadmitted},                        // a member's plain re-add (G020 5)
      {alice.Add(erin.Id()), Reason::NotReadmitted},                        // even the owner names the removal
      {bob.Add(erin.Id(), "invite-1", "removal-2"), Reason::NotReadmitted}, // not this removal
  };
  for (const auto& [change, reason] : refused) {
    view = Compute(s, {change});
    EXPECT_FALSE(view.Lists(erin.Id()));
    EXPECT_TRUE(Is(view, change, Status::Void, reason));
    ASSERT_EQ(view.removed.size(), 1u) << "still removed";
  }
}

TEST_F(GroupRosterRulesTest, AReadmitMayNameARemovalNotYetFolded) {
  const GroupRosterStatement s = Statement([&](GroupRosterStatement& d) { d.members.push_back(Row(erin.Id())); });
  const GroupChange removal = bob.Remove(erin.Id());
  const GroupChange readmit = alice.Add(erin.Id(), "invite-1", grc::Id(removal));
  EXPECT_TRUE(Compute(s, {removal, readmit}).Lists(erin.Id()));
  EXPECT_FALSE(Compute(s, {removal}).Lists(erin.Id()));

  const GroupChange members = carol.Add(erin.Id(), "invite-1", grc::Id(removal));
  EXPECT_TRUE(Is(Compute(s, {removal, members}), members, Status::Void, Reason::RemovedMember));
  const GroupChange names_nothing = alice.Add(erin.Id(), "invite-1", "no-such-removal");
  EXPECT_TRUE(Is(Compute(s, {names_nothing}), names_nothing, Status::Void, Reason::NotReadmitted));
}

TEST_F(GroupRosterRulesTest, TheOwnerRemovesAnyoneButItselfAnAdminOnlyMembers) {
  // Dave is a second admin.
  const GroupRosterStatement s =
      Statement([&](GroupRosterStatement& d) { d.members[3] = Row(dave.Id(), MemberRole::Admin); });
  const auto removed = [&](const GroupChange& r) { return Is(Compute(s, {r}), r, Status::Applied); };
  const auto refused = [&](const GroupChange& r) { return Is(Compute(s, {r}), r, Status::Void, Reason::NotAllowed); };
  EXPECT_TRUE(removed(bob.Remove(carol.Id())));
  EXPECT_TRUE(refused(bob.Remove(dave.Id()))) << "an admin does not remove an admin";
  EXPECT_TRUE(refused(bob.Remove(alice.Id())));
  EXPECT_TRUE(refused(carol.Remove(erin.Id()))) << "members do not remove";
  EXPECT_TRUE(removed(alice.Remove(bob.Id())));
  EXPECT_TRUE(removed(alice.Remove(dave.Id())));
  EXPECT_FALSE(Compute(s, {alice.Remove(bob.Id())}).Lists(bob.Id()));
}

// A removal needs no member to act on: it still bars members' adds of that account (an admin's power; G020 has no
// ban list, and this is not one: an owner or admin re-admits).
TEST_F(GroupRosterRulesTest, ARemovalOfSomeoneNotInTheGroupStillBarsMembersAdds) {
  const GroupChange removal = bob.Remove("account:zed");
  const GroupChange add = carol.Add("account:zed");
  const GroupRosterView view = Compute({removal, add});
  EXPECT_FALSE(view.Lists("account:zed"));
  EXPECT_TRUE(Is(view, add, Status::Void, Reason::RemovedMember));
  ASSERT_EQ(view.removed.size(), 1u);
}

// ---- R3: a change counts while its signer may make it, against the latest statement ----

TEST_F(GroupRosterRulesTest, ADemotedAdminsBackdatedRemovalIsVoid) {
  const GroupChange removal = bob.Remove(carol.Id()); // base_epoch 5: "made while I was admin"
  ASSERT_EQ(removal.base_epoch, 5u);
  EXPECT_FALSE(Compute(Statement(), {removal}).Lists(carol.Id())) << "Bob as admin removes Carol";

  const GroupRosterStatement demoted = Statement([](GroupRosterStatement& d) {
    d.roster_epoch = 6;
    d.members[1].role = MemberRole::Member;
  });
  const GroupRosterView view = Compute(demoted, {removal});
  EXPECT_TRUE(view.Lists(carol.Id())) << "judged against the latest statement, not the one it names";
  EXPECT_TRUE(Is(view, removal, Status::Void, Reason::NotAllowed));
}

TEST_F(GroupRosterRulesTest, ARemovedInvitersAddsAreKeptByTheRemoversKeeps) {
  const GroupChange add = carol.Add(erin.Id());
  const GroupChange keeping = bob.Remove(carol.Id(), {}, {grc::Id(add)});
  GroupRosterView view = Compute({add, keeping});
  EXPECT_FALSE(view.Lists(carol.Id()));
  ASSERT_TRUE(view.Lists(erin.Id())) << "G019 5: removing an inviter keeps the people they brought";
  EXPECT_EQ(view.Find(erin.Id())->added_by, carol.Id());
  EXPECT_TRUE(Is(view, add, Status::Applied));

  const GroupChange dropping = bob.Remove(carol.Id());
  view = Compute({add, dropping});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(Is(view, add, Status::Void, Reason::SignerOut));
}

TEST_F(GroupRosterRulesTest, ALeaversOwnAddsAreKeptByItsLeave) {
  const GroupChange add = carol.Add(erin.Id());
  GroupRosterView view = Compute({add, carol.Leave({}, {grc::Id(add)})});
  EXPECT_FALSE(view.Lists(carol.Id()));
  EXPECT_TRUE(view.Lists(erin.Id()));

  view = Compute({add, carol.Leave()});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(Is(view, add, Status::Void, Reason::SignerOut));
}

TEST_F(GroupRosterRulesTest, ARemovedMembersLeaveKeepsNothingTheRemoverDecides) {
  const GroupChange add = carol.Add(erin.Id());
  const GroupRosterView view = Compute({add, bob.Remove(carol.Id()), carol.Leave({}, {grc::Id(add)})});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(Is(view, add, Status::Void, Reason::SignerOut));
}

TEST_F(GroupRosterRulesTest, KeepsCoverOnlyTheSignersOwnChanges) {
  const GroupChange daves = dave.Add(erin.Id());
  // Carol's leave lists Dave's add; Dave left without keeping it.
  GroupRosterView view = Compute({daves, dave.Leave(), carol.Leave({}, {grc::Id(daves)})});
  EXPECT_FALSE(view.Lists(erin.Id()));
  // A removal of Carol lists Dave's add; Dave was removed too, keeping nothing.
  view = Compute({daves, bob.Remove(dave.Id()), bob.Remove(carol.Id(), {}, {grc::Id(daves)})});
  EXPECT_FALSE(view.Lists(erin.Id()));
}

TEST_F(GroupRosterRulesTest, ANewcomerBringsOthersInAndTheyFallWithThem) {
  const GroupChange brings_erin = carol.Add(erin.Id());
  const GroupChange brings_frank = erin.Add(frank.Id());
  GroupRosterView view = Compute({brings_frank, brings_erin});
  ASSERT_TRUE(view.Lists(frank.Id()));
  EXPECT_EQ(view.Find(frank.Id())->added_by, erin.Id());

  view = Compute({brings_frank, brings_erin, bob.Remove(erin.Id(), {grc::Id(brings_erin)})});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_FALSE(view.Lists(frank.Id()));
  EXPECT_TRUE(Is(view, brings_frank, Status::Void, Reason::SignerOut));

  // Two strangers adding each other bring nobody in.
  const GroupChange frank_brings_erin = frank.Add(erin.Id());
  view = Compute({brings_frank, frank_brings_erin});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_FALSE(view.Lists(frank.Id()));
  EXPECT_TRUE(Is(view, brings_frank, Status::Void, Reason::SignerNotJoined));
  EXPECT_FALSE(FoldsIn(*view.Verdict(grc::Id(brings_frank)))) << "Erin's own add may still arrive";
}

// Stated limit 1: a change by someone the owner removes at the same moment, that nobody kept, is dropped.
TEST_F(GroupRosterRulesTest, TheOwnerRemovingAnAdminDropsTheAdminsUnkeptRemoval) {
  const GroupChange bobs = bob.Remove(dave.Id());
  GroupRosterView view = Compute({bobs, alice.Remove(bob.Id())});
  EXPECT_TRUE(view.Lists(dave.Id()));
  EXPECT_TRUE(Is(view, bobs, Status::Void, Reason::SignerOut));

  view = Compute({bobs, alice.Remove(bob.Id(), {}, {grc::Id(bobs)})});
  EXPECT_FALSE(view.Lists(dave.Id())) << "the owner kept Bob's removal";
}

// ---- R4: renames ----

TEST_F(GroupRosterRulesTest, RenamesTakeTheHighestVersionThenTheHigherRoleThenTheLowerId) {
  const GroupChange carols = carol.Rename("Trail", 6);
  const GroupChange daves = dave.Rename("Peak", 6);
  GroupRosterView view = Compute({carols, daves});
  const bool carol_lower = grc::Id(carols) < grc::Id(daves);
  EXPECT_EQ(view.title, carol_lower ? "Trail" : "Peak");
  EXPECT_EQ(view.title_version, 6u);
  EXPECT_TRUE(Is(view, carol_lower ? daves : carols, Status::Superseded, Reason::LostRename));

  const GroupChange bobs = bob.Rename("Summit", 6);
  EXPECT_EQ(Compute({carols, daves, bobs}).title, "Summit") << "an admin's rename outranks members' at one version";
  EXPECT_EQ(Compute({carols, bobs, alice.Rename("Base", 6)}).title, "Base") << "the owner outranks an admin";
  EXPECT_EQ(Compute({bobs, carol.Rename("Ridge", 7)}).title, "Ridge") << "a higher version wins over a role";

  const GroupChange stale = carol.Rename("Old", 5);
  view = Compute({stale});
  EXPECT_EQ(view.title, "Hikers") << "not above the statement's version";
  EXPECT_TRUE(Is(view, stale, Status::Superseded, Reason::OlderTitle));
}

// ---- R5: 50 members ----

TEST_F(GroupRosterRulesTest, AddsPastFiftyAreVoidInChangeIdOrder) {
  const GroupRosterStatement s = Statement([&](GroupRosterStatement& d) {
    for (int i = 0; d.members.size() < 48; ++i) {
      d.members.push_back(Row("account:filler-" + std::to_string(i)));
    }
  });
  std::vector<GroupChange> adds;
  for (const char* who : {"account:n1", "account:n2", "account:n3", "account:n4"}) {
    adds.push_back(alice.Add(who));
  }
  std::vector<std::string> ids;
  for (const GroupChange& a : adds) {
    ids.push_back(grc::Id(a));
  }
  std::vector<std::string> sorted = ids;
  std::sort(sorted.begin(), sorted.end());

  GroupRosterView view = Compute(s, adds);
  EXPECT_EQ(view.members.size(), kMaxGroupMembers);
  for (size_t i = 0; i < adds.size(); ++i) {
    const auto rank = static_cast<size_t>(std::find(sorted.begin(), sorted.end(), ids[i]) - sorted.begin());
    EXPECT_EQ(view.Lists(adds[i].member), rank < 2) << adds[i].member;
    if (rank >= 2) {
      EXPECT_TRUE(Is(view, adds[i], Status::Void, Reason::GroupFull));
      EXPECT_TRUE(FoldsIn(*view.Verdict(ids[i]))) << "the signer sends group_invite_refused; it stays void";
    }
  }

  // Dave's leave frees a place: three of the four fit.
  std::vector<GroupChange> with_leave = adds;
  with_leave.push_back(dave.Leave());
  view = Compute(s, with_leave);
  EXPECT_EQ(view.members.size(), kMaxGroupMembers);
  size_t in = 0;
  for (const GroupChange& a : adds) {
    in += view.Lists(a.member) ? 1 : 0;
  }
  EXPECT_EQ(in, 3u);
}

// ---- R6: a device off the card ----

TEST_F(GroupRosterRulesTest, AChangeFromADeviceTakenOffTheCardCountsOnlyIfFoldedOrKept) {
  const AccountCard both = carol.Card({"1", "2"}, 1000);
  const AccountCard newer = carol.Card({"1"}, 2000);
  const GroupChange bare = carol.Add(erin.Id(), "invite-1", "", "2");

  book.Hold(both);
  EXPECT_TRUE(Compute({bare}).Lists(erin.Id())) << "this device holds only the card that lists device 2";

  book.Hold(newer);
  GroupRosterView view = Compute({bare});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(Is(view, bare, Status::Void, Reason::NoCard)) << "no card this device has proves device 2 was hers";

  // Carrying the older card that lists device 2 proves it was hers; the newer card, held, says it no longer is.
  GroupChange from_two = bare;
  from_two.signer_card = both;
  view = Compute({from_two});
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(Is(view, from_two, Status::Void, Reason::DeviceOffCard));
  EXPECT_TRUE(FoldsIn(*view.Verdict(grc::Id(from_two))));

  // The newer card can also arrive with a change instead of the book.
  book.Hold(both);
  GroupChange carrying = carol.Rename("Trail", 6);
  carrying.signer_card = newer;
  EXPECT_TRUE(Is(Compute({from_two, carrying}), from_two, Status::Void, Reason::DeviceOffCard));
  book.Hold(newer);

  // Folded: the owner took it in while the device was hers.
  const GroupRosterStatement folded = Statement([&](GroupRosterStatement& d) {
    d.members.push_back(Row(erin.Id(), MemberRole::Member, carol.Id()));
    d.folded = {{.id = grc::Id(from_two), .base_epoch = from_two.base_epoch}};
  });
  view = Compute(folded, {from_two});
  EXPECT_TRUE(view.Lists(erin.Id()));
  EXPECT_TRUE(Is(view, from_two, Status::Dropped, Reason::Folded));

  // Kept by a removal of Carol, or by Carol's own leave from a device still on her card.
  EXPECT_TRUE(Compute({from_two, bob.Remove(carol.Id(), {}, {grc::Id(from_two)})}).Lists(erin.Id()));
  EXPECT_TRUE(Compute({from_two, carol.Leave({}, {grc::Id(from_two)})}).Lists(erin.Id()));
  const GroupChange leave_from_two = carol.Leave({}, {grc::Id(from_two)}, "2");
  view = Compute({from_two, leave_from_two});
  EXPECT_TRUE(Is(view, leave_from_two, Status::Void, Reason::DeviceOffCard));
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(view.Lists(carol.Id()));
}

TEST_F(GroupRosterRulesTest, ACarriedCardProvesAChangeThisDeviceHasNoCardFor) {
  book.cards.erase(erin.Id());
  const GroupChange brings_erin = carol.Add(erin.Id());
  GroupChange brings_frank = erin.Add(frank.Id());
  GroupRosterView view = Compute({brings_erin, brings_frank});
  EXPECT_TRUE(Is(view, brings_frank, Status::Void, Reason::NoCard));
  EXPECT_FALSE(view.Lists(frank.Id()));
  const std::vector<std::string> held = view.HeldIds();
  EXPECT_NE(std::find(held.begin(), held.end(), grc::Id(brings_frank)), held.end()) << "held until the card arrives";
  EXPECT_FALSE(FoldsIn(*view.Verdict(grc::Id(brings_frank))));

  brings_frank.signer_card = frank.Card();
  EXPECT_TRUE(Is(Compute({brings_erin, brings_frank}), brings_frank, Status::Void, Reason::NoCard))
      << "another account's card proves nothing for Erin";
  brings_frank.signer_card = erin.Card();
  view = Compute({brings_erin, brings_frank});
  EXPECT_TRUE(view.Lists(frank.Id()));
}

TEST_F(GroupRosterRulesTest, AForgedCopyIsDroppedAndLosesToTheRealOne) {
  const GroupChange real = carol.Add(erin.Id());
  GroupChange forged = real;
  forged.signature_b64 = carol.Add("account:mallory").signature_b64;
  ASSERT_EQ(grc::Id(forged), grc::Id(real)) << "the id is of the signed bytes, not the signature";

  GroupRosterView view = Compute({forged});
  EXPECT_TRUE(Is(view, forged, Status::Dropped, Reason::BadSignature));
  EXPECT_TRUE(view.HeldIds().empty());
  EXPECT_FALSE(FoldsIn(*view.Verdict(grc::Id(forged)))) << "folding its id would drop the real change";

  view = Compute({forged, real, real});
  EXPECT_TRUE(view.Lists(erin.Id()));
  EXPECT_EQ(view.verdicts.size(), 1u) << "copies of one change are one change";
}

// ---- G017 presets ----

TEST_F(GroupRosterRulesTest, AManagedGroupStopsMembersAddsRenamesAndLinks) {
  const GroupRosterStatement managed = Statement([](GroupRosterStatement& d) { d.preset = GroupPreset::Managed; });
  for (const GroupChange& c : {carol.Add(erin.Id()), carol.Rename("Trail", 6), carol.LinkAdd("link-1")}) {
    EXPECT_TRUE(Is(Compute(managed, {c}), c, Status::Void, Reason::NotAllowed)) << grc::KindToString(c.kind);
  }
  const GroupChange admins = bob.Add(erin.Id());
  const GroupChange admins_link = bob.LinkAdd("link-2");
  const GroupRosterView view = Compute(managed, {admins, admins_link, bob.Rename("Trail", 6)});
  EXPECT_TRUE(view.Lists(erin.Id()));
  EXPECT_EQ(view.links.size(), 1u);
  EXPECT_EQ(view.title, "Trail");
  EXPECT_TRUE(Is(Compute(managed, {carol.Leave()}), carol.Leave(), Status::Applied)) << "anyone leaves";
}

// ---- Links (G021) ----

TEST_F(GroupRosterRulesTest, ALinkLivesWhileItsMakerIsInTheGroupAndUntilItExpires) {
  const GroupChange link = carol.LinkAdd("link-1");
  GroupRosterView view = Compute({link});
  ASSERT_EQ(view.links.size(), 1u);
  EXPECT_EQ(view.links[0].maker, carol.Id());
  EXPECT_EQ(view.links[0].secret_hash, "hash-of-link-1");

  EXPECT_TRUE(Compute({link, carol.Leave()}).links.empty()) << "the maker left";
  EXPECT_TRUE(Compute({link, bob.Remove(carol.Id())}).links.empty()) << "the maker was removed";
  EXPECT_TRUE(Compute({carol.LinkAdd("link-2", kNowMs - 1)}).links.empty()) << "expired";

  // A link in the statement dies with its maker too.
  const GroupRosterStatement s = Statement([&](GroupRosterStatement& d) {
    d.links = {{.link_id = "link-0", .secret_hash = "h", .maker = dave.Id(), .expires_at = kNowMs + 1000}};
  });
  EXPECT_EQ(Compute(s, {}).links.size(), 1u);
  EXPECT_TRUE(Compute(s, {dave.Leave()}).links.empty());
}

TEST_F(GroupRosterRulesTest, ItsMakerAdminsAndTheOwnerRevokeALink) {
  const GroupChange link = carol.LinkAdd("link-1");
  for (Person* revoker : {&carol, &bob, &alice}) {
    const GroupChange revoke = revoker->LinkRevoke("link-1");
    const GroupRosterView view = Compute({link, revoke});
    EXPECT_TRUE(view.links.empty()) << revoker->Name();
    EXPECT_TRUE(Is(view, revoke, Status::Applied));
  }
  const GroupChange daves = dave.LinkRevoke("link-1");
  GroupRosterView view = Compute({link, daves});
  EXPECT_EQ(view.links.size(), 1u);
  EXPECT_TRUE(Is(view, daves, Status::Void, Reason::NotAllowed));

  const GroupChange unknown = bob.LinkRevoke("link-9");
  view = Compute({unknown});
  EXPECT_TRUE(Is(view, unknown, Status::Void, Reason::NoSuchLink));
  EXPECT_FALSE(FoldsIn(*view.Verdict(grc::Id(unknown)))) << "its link_add may still arrive";
}

TEST_F(GroupRosterRulesTest, AnEndedLinkSaysWhyItNoLongerWorks) {
  const GroupChange link = carol.LinkAdd("link-1");
  const auto end_of = [](const GroupRosterView& view) -> std::optional<GroupLinkEnd> {
    const GroupEndedLink* ended = view.EndedLink("link-1");
    return ended ? std::optional<GroupLinkEnd>(ended->end) : std::nullopt;
  };
  EXPECT_FALSE(end_of(Compute({link}))) << "live";
  EXPECT_NE(Compute({link}).LiveLink("link-1"), nullptr);
  EXPECT_EQ(end_of(Compute({link, bob.LinkRevoke("link-1")})), GroupLinkEnd::Revoked);
  EXPECT_EQ(end_of(Compute({link, carol.Leave()})), GroupLinkEnd::MakerOut) << "it dies with its maker";
  EXPECT_EQ(end_of(Compute({link, bob.Remove(carol.Id())})), GroupLinkEnd::MakerOut);
  EXPECT_EQ(Compute({carol.LinkAdd("link-1", kNowMs - 1)}).EndedLink("link-1")->end, GroupLinkEnd::Expired);
  const GroupChange kept_by_leave = carol.Leave({}, {grc::Id(link)});
  EXPECT_EQ(end_of(Compute({link, kept_by_leave})), GroupLinkEnd::MakerOut) << "its link_add kept, the link still dies";
  EXPECT_EQ(end_of(Compute({link, kept_by_leave, bob.LinkRevoke("link-1")})), GroupLinkEnd::Revoked)
      << "a revoke says so first";
  const GroupRosterStatement s = Statement([&](GroupRosterStatement& d) {
    d.links = {{.link_id = "link-1", .secret_hash = "h", .maker = carol.Id(), .expires_at = kNowMs + 1000}};
  });
  EXPECT_EQ(Compute(s, {carol.Leave()}).EndedLink("link-1")->end, GroupLinkEnd::MakerOut) << "a folded one too";
}

TEST_F(GroupRosterRulesTest, ALinksUsesAreTheAddsCarryingItThatTookTheirMemberIn) {
  const GroupChange link = carol.LinkAdd("link-1");
  const GroupChange erins = carol.AddByLink(erin.Id(), "link-1");
  const GroupChange franks = carol.AddByLink(frank.Id(), "link-1");
  GroupRosterView view = Compute({link, erins, franks});
  ASSERT_NE(view.LiveLink("link-1"), nullptr);
  EXPECT_EQ(view.LiveLink("link-1")->uses, 2u);
  EXPECT_TRUE(view.Lists(erin.Id()));
  EXPECT_EQ(view.Find(erin.Id())->added_by, carol.Id()) << "who answered the link brought them";

  // One who left still used it; an add a removal undid did not take anyone in for long, and is not counted.
  view = Compute({link, erins, franks, erin.Leave({grc::Id(erins)}), bob.Remove(frank.Id(), {grc::Id(franks)})});
  EXPECT_EQ(view.LiveLink("link-1")->uses, 1u);

  // The fold carries the count into the statement, so the adds it drops still count.
  const GroupRosterStatement s = Statement();
  const GroupRosterStatement folded = FoldChanges(s, Compute(s, {link, erins}));
  ASSERT_EQ(folded.links.size(), 1u);
  EXPECT_EQ(folded.links[0].uses, 1u);
  const GroupRosterStatement signed_fold = test::SignedStatement(alice, folded);
  view = Compute(signed_fold, {link, erins, franks});
  EXPECT_EQ(view.LiveLink("link-1")->uses, 2u) << "the statement's one plus Frank's, not Erin's again";
}

TEST_F(GroupRosterRulesTest, ALinkIdIsTakenOnce) {
  const GroupChange carols = carol.LinkAdd("link-1");
  const GroupChange daves = dave.LinkAdd("link-1");
  const GroupRosterView view = Compute({carols, daves});
  const bool carol_lower = grc::Id(carols) < grc::Id(daves);
  ASSERT_EQ(view.links.size(), 1u);
  EXPECT_EQ(view.links[0].maker, carol_lower ? carol.Id() : dave.Id());
  EXPECT_TRUE(Is(view, carol_lower ? daves : carols, Status::Void, Reason::DuplicateLink));

  const GroupRosterStatement s = Statement([&](GroupRosterStatement& d) {
    d.links = {{.link_id = "link-1", .secret_hash = "h", .maker = bob.Id(), .expires_at = kNowMs + 1000}};
  });
  EXPECT_TRUE(Is(Compute(s, {carols}), carols, Status::Void, Reason::DuplicateLink));
}

// ---- What is dropped ----

TEST_F(GroupRosterRulesTest, ChangesForAnotherGroupOrAlreadyFoldedAreDropped) {
  GroupChange other = carol.Draft(Kind::Add);
  other.group_id = "group:other";
  other.member = erin.Id();
  other.invite_nonce = "n";
  other = carol.Signed(other);
  const GroupChange folded = carol.Add(frank.Id());
  const GroupRosterStatement s = Statement(
      [&](GroupRosterStatement& d) { d.folded = {{.id = grc::Id(folded), .base_epoch = folded.base_epoch}}; });
  const GroupRosterView view = Compute(s, {other, folded});
  EXPECT_TRUE(Is(view, other, Status::Dropped, Reason::OtherGroup));
  EXPECT_TRUE(Is(view, folded, Status::Dropped, Reason::Folded));
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_FALSE(view.Lists(frank.Id())) << "folded means the statement holds its effect; this one has none";
  EXPECT_TRUE(view.HeldIds().empty());
}

TEST_F(GroupRosterRulesTest, ADissolvedGroupHasNoRoster) {
  const GroupRosterStatement dissolved = Statement([](GroupRosterStatement& d) { d.dissolved = true; });
  const GroupChange add = carol.Add(erin.Id());
  const GroupRosterView view = Compute(dissolved, {add});
  EXPECT_TRUE(view.dissolved);
  EXPECT_TRUE(view.members.empty());
  EXPECT_TRUE(Is(view, add, Status::Dropped, Reason::Dissolved));
}

// ---- G018 item 4: the owner folds ----

std::vector<GroupChange> MixedChanges(const Person& alice, const Person& bob, const Person& carol, const Person& dave,
                                      const Person& erin, const Person& frank) {
  const GroupChange brings_erin = carol.Add(erin.Id());
  const GroupChange brings_frank = erin.Add(frank.Id());
  return {
      brings_erin,
      brings_frank,
      bob.Remove(carol.Id(), {}, {grc::Id(brings_erin)}), // Carol out, Erin kept
      dave.Add(erin.Id(), "invite-2"),                    // a second add of someone already in
      frank.Leave({grc::Id(brings_frank)}),               // Frank leaves
      carol.Rename("Trail", 6),                           // Carol is out: void
      dave.Rename("Peak", 6),
      erin.LinkAdd("link-1"),
      alice.Add("account:gina", "invite-9", "removal-9"), // names no removal: void
  };
}

TEST_F(GroupRosterRulesTest, FoldingThenComputingGivesTheSameRoster) {
  const GroupRosterStatement s = Statement();
  std::vector<GroupChange> changes = MixedChanges(alice, bob, carol, dave, erin, frank);
  book.cards.erase(Named("gina").Id());
  const GroupChange no_card = Named("gina").Leave(); // unknown card and signer: stays unfolded
  changes.push_back(no_card);

  const GroupRosterView before = Compute(s, changes);
  ASSERT_TRUE(before.Lists(erin.Id()));
  ASSERT_FALSE(before.Lists(frank.Id()));
  ASSERT_FALSE(before.Lists(carol.Id()));
  ASSERT_EQ(before.title, "Peak");
  ASSERT_EQ(before.links.size(), 1u);

  const GroupRosterStatement next_draft = FoldChanges(s, before);
  EXPECT_EQ(next_draft.roster_epoch, s.roster_epoch + 1);
  const GroupRosterStatement next = test::SignedStatement(alice, next_draft);
  ASSERT_TRUE(group_roster_statement::Verify(next, alice.Id()));
  EXPECT_EQ(next.preset, s.preset);

  const GroupRosterView after = Compute(next, changes);
  EXPECT_EQ(RosterOf(after), RosterOf(before));
  for (const GroupChangeVerdict& v : before.verdicts) {
    const GroupChangeVerdict* folded = after.Verdict(v.id);
    ASSERT_TRUE(folded);
    if (FoldsIn(v)) {
      EXPECT_EQ(folded->status, Status::Dropped);
      EXPECT_EQ(folded->reason, Reason::Folded);
    }
  }
  EXPECT_TRUE(Is(after, no_card, Status::Void, Reason::NoCard)) << "not folded: it may count once its card arrives";
  EXPECT_EQ(after.HeldIds(), std::vector<std::string>{grc::Id(no_card)});

  // A newcomer with only the new statement sees the same group.
  EXPECT_EQ(RosterOf(Compute(next, {})), RosterOf(before));
}

TEST_F(GroupRosterRulesTest, AFoldCarriesEarlierFoldsForward) {
  const GroupRosterStatement s =
      Statement([](GroupRosterStatement& d) { d.folded = {{.id = "earlier", .base_epoch = 3}}; });
  const GroupChange add = carol.Add(erin.Id());
  const GroupRosterStatement next = FoldChanges(s, Compute(s, {add}));
  std::vector<GroupFoldedChange> expected{{.id = grc::Id(add), .base_epoch = add.base_epoch},
                                          {.id = "earlier", .base_epoch = 3}};
  std::sort(expected.begin(), expected.end(),
            [](const GroupFoldedChange& a, const GroupFoldedChange& b) { return a.id < b.id; });
  EXPECT_EQ(next.folded, expected);
  ASSERT_EQ(next.members.size(), 5u);
  const auto erin_row = std::find_if(next.members.begin(), next.members.end(),
                                     [&](const GroupRosterStatementMember& m) { return m.identity == erin.Id(); });
  ASSERT_NE(erin_row, next.members.end());
  EXPECT_EQ(erin_row->added_by, carol.Id());
  EXPECT_EQ(erin_row->joined_at, add.at);
}

// Why the owner folds void changes too: a demoted admin's backdated removal must stay void if the owner later
// makes them admin again.
TEST_F(GroupRosterRulesTest, FoldingAVoidChangeKeepsItVoidAfterAReappointment) {
  const GroupChange backdated = bob.Remove(carol.Id());
  const GroupRosterStatement demoted = Statement([](GroupRosterStatement& d) {
    d.roster_epoch = 6;
    d.members[1].role = MemberRole::Member;
  });
  const GroupRosterView at_six = Compute(demoted, {backdated});
  ASSERT_TRUE(Is(at_six, backdated, Status::Void, Reason::NotAllowed));
  GroupRosterStatement reappointed = FoldChanges(demoted, at_six);
  for (GroupRosterStatementMember& m : reappointed.members) {
    if (m.identity == bob.Id()) {
      m.role = MemberRole::Admin;
    }
  }
  reappointed = test::SignedStatement(alice, reappointed);
  EXPECT_TRUE(Compute(reappointed, {backdated}).Lists(carol.Id()));

  const GroupRosterStatement unfolded = Statement([](GroupRosterStatement& d) { d.roster_epoch = 7; });
  EXPECT_FALSE(Compute(unfolded, {backdated}).Lists(carol.Id())) << "unfolded, it would count again";
}

// ---- The change floor (G018: `folded` bounded) ----

TEST_F(GroupRosterRulesTest, AChangeMadeAgainstAStatementBelowTheFloorIsDroppedAsTooOld) {
  GroupChange old_draft = carol.Draft(GroupChangeKind::Add);
  old_draft.member = erin.Id();
  old_draft.invite_nonce = "invite-1";
  old_draft.base_epoch = 3;
  const GroupChange old_add = carol.Signed(old_draft);
  const GroupChange fresh = dave.Add(frank.Id()); // base epoch 5
  const GroupRosterStatement s = Statement([](GroupRosterStatement& d) {
    d.roster_epoch = 40;
    d.change_floor_epoch = 4;
  });
  const GroupRosterView view = Compute(s, {old_add, fresh});
  EXPECT_TRUE(Is(view, old_add, Status::Dropped, Reason::TooOld)) << "lost: it must be made again";
  EXPECT_FALSE(view.Lists(erin.Id()));
  EXPECT_TRUE(view.Lists(frank.Id())) << "at the floor or above, a change still counts";
  EXPECT_EQ(view.HeldIds(), std::vector<std::string>{grc::Id(fresh)}) << "a device drops what is too old";
}

TEST_F(GroupRosterRulesTest, FoldingRaisesTheFloorAndTrimsWhatItFolded) {
  const GroupChange add = carol.Add(erin.Id()); // base epoch 5
  GroupRosterStatement s = Statement([](GroupRosterStatement& d) {
    d.roster_epoch = 36;
    d.change_floor_epoch = 1;
    d.folded = {{.id = "two", .base_epoch = 2}, {.id = "four", .base_epoch = 4}, {.id = "six", .base_epoch = 6}};
  });
  const GroupRosterStatement next = FoldChanges(s, Compute(s, {add}));
  EXPECT_EQ(next.roster_epoch, 37u);
  EXPECT_EQ(next.change_floor_epoch, 37u - kChangeFloorLag);
  std::vector<std::string> ids;
  for (const GroupFoldedChange& f : next.folded) {
    ids.push_back(f.id);
    EXPECT_GE(f.base_epoch, next.change_floor_epoch);
  }
  std::vector<std::string> expected{grc::Id(add), "six"};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(ids, expected) << "two and four are below the floor: dropped as too old anyway";
  const GroupRosterStatement signed_next = test::SignedStatement(alice, next);
  ASSERT_TRUE(group_roster_statement::Verify(signed_next, alice.Id()));
  EXPECT_TRUE(Is(Compute(signed_next, {add}), add, Status::Dropped, Reason::Folded));

  // The floor never goes down, and a young group has none.
  GroupRosterStatement young = Statement();
  EXPECT_EQ(FoldChanges(young, Compute(young, {})).change_floor_epoch, 0u);
  s.change_floor_epoch = 30;
  s.folded.clear();
  EXPECT_EQ(FoldChanges(s, Compute(s, {})).change_floor_epoch, 30u);
}

TEST_F(GroupRosterRulesTest, AFoldRecordsAFutureBaseEpochAsTheStatementsOwn) {
  GroupChange draft = carol.Draft(GroupChangeKind::Add);
  draft.member = erin.Id();
  draft.invite_nonce = "invite-1";
  draft.base_epoch = 1'000'000; // no statement that new exists
  const GroupChange future = carol.Signed(draft);
  const GroupRosterStatement s = Statement();
  const GroupRosterStatement next = FoldChanges(s, Compute(s, {future}));
  ASSERT_EQ(next.folded.size(), 1u);
  EXPECT_EQ(next.folded[0].base_epoch, s.roster_epoch) << "so the floor trims it in time";
}

// ---- The core property: order independence ----

TEST_F(GroupRosterRulesTest, EveryOrderOfTheSameChangesGivesTheSameRoster) {
  const GroupRosterStatement s = Statement();
  std::vector<GroupChange> changes = MixedChanges(alice, bob, carol, dave, erin, frank);
  changes.pop_back(); // eight changes: every one of the 40320 orders
  ASSERT_EQ(changes.size(), 8u);
  const std::vector<CheckedGroupChange> checked = CheckChanges(changes, book.Lookup(), kNowMs);
  const std::string expected = test::Describe(ComputeRosterChecked(s, checked, kNowMs));
  ASSERT_NE(expected.find(erin.Id()), std::string::npos);

  std::vector<size_t> order(checked.size());
  std::iota(order.begin(), order.end(), 0);
  size_t orders = 0;
  do {
    std::vector<CheckedGroupChange> permuted;
    for (const size_t i : order) {
      permuted.push_back(checked[i]);
    }
    ASSERT_EQ(test::Describe(ComputeRosterChecked(s, permuted, kNowMs)), expected) << "order " << orders;
    ++orders;
  } while (std::next_permutation(order.begin(), order.end()));
  EXPECT_EQ(orders, 40320u);

  // And through the signature checks, in a sample of orders.
  std::mt19937 rng(20261105);
  for (int i = 0; i < 20; ++i) {
    std::shuffle(changes.begin(), changes.end(), rng);
    EXPECT_EQ(test::Describe(Compute(s, changes)), expected);
  }
}

TEST_F(GroupRosterRulesTest, EveryOrderGivesTheSameRosterAtTheSizeLimitWithKeepsAndRemovals) {
  const GroupRosterStatement s = Statement([&](GroupRosterStatement& d) {
    for (int i = 0; d.members.size() < 48; ++i) {
      d.members.push_back(Row("account:filler-" + std::to_string(i)));
    }
  });
  const GroupChange brings_erin = carol.Add(erin.Id());
  const GroupChange brings_frank = dave.Add(frank.Id());
  const std::vector<GroupChange> changes = {
      brings_erin,
      brings_frank,
      alice.Add("account:gina"),
      bob.Remove(dave.Id(), {}, {grc::Id(brings_frank)}),
      erin.Add("account:hank"),
      carol.Leave({}, {grc::Id(brings_erin)}),
      dave.Add("account:ivy"),
  };
  const std::vector<CheckedGroupChange> checked = CheckChanges(changes, book.Lookup(), kNowMs);
  const GroupRosterView baseline = ComputeRosterChecked(s, checked, kNowMs);
  EXPECT_EQ(baseline.members.size(), kMaxGroupMembers);
  const std::string expected = test::Describe(baseline);

  std::vector<size_t> order(checked.size());
  std::iota(order.begin(), order.end(), 0);
  do {
    std::vector<CheckedGroupChange> permuted;
    for (const size_t i : order) {
      permuted.push_back(checked[i]);
    }
    ASSERT_EQ(test::Describe(ComputeRosterChecked(s, permuted, kNowMs)), expected);
  } while (std::next_permutation(order.begin(), order.end()));
}

TEST_F(GroupRosterRulesTest, TheChangesHashIsOfWhatADeviceHolds) {
  const std::vector<GroupChange> changes = MixedChanges(alice, bob, carol, dave, erin, frank);
  std::vector<GroupChange> reversed(changes.rbegin(), changes.rend());
  const GroupRosterView a = Compute(changes);
  const GroupRosterView b = Compute(reversed);
  EXPECT_EQ(grc::ChangesHash(a.HeldIds()), grc::ChangesHash(b.HeldIds()));
  EXPECT_EQ(a.HeldIds().size(), changes.size());
  std::vector<GroupChange> fewer(changes.begin(), changes.end() - 1);
  EXPECT_NE(grc::ChangesHash(Compute(fewer).HeldIds()), grc::ChangesHash(a.HeldIds()));
}

} // namespace
} // namespace pbr
