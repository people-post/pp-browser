#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/GroupInviteGate.h"

#include "common/Utilities.h"

#include <filesystem>
#include <map>
#include <memory>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

GroupInvitePayload MakeInvite(const std::string& inviter = "account:alice") {
  GroupInvitePayload invite;
  invite.group_id = "group:test";
  invite.group_title = "Test";
  invite.inviter_identity = inviter;
  invite.invitee_identity = "account:bob";
  invite.invite_nonce = util::GenerateUuid();
  invite.roster_epoch = 1;
  invite.expires_at = util::NowUnixMs() + 7LL * 24 * 60 * 60 * 1000;
  return invite;
}

class GroupInviteGateTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_invite_gate_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
    store_ = std::make_unique<InMemoryDeliveryStore>();
    ASSERT_TRUE(store_->ListThreads());
    roster_ = std::make_unique<InMemoryGroupRosterStore>(*store_);
    gate_ = std::make_unique<GroupInviteGate>(
        [this](const std::string& identity) {
          auto it = standings_.find(identity);
          return it == standings_.end() ? GroupInviteGate::InviterStanding::Unknown : it->second;
        },
        *roster_);
  }

  void TearDown() override {
    gate_.reset();
    roster_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  /** As people would say: a contact, or a blocked one. */
  void AddContact(const std::string& relay_id, const bool blocked = false) {
    standings_[relay_id] =
        blocked ? GroupInviteGate::InviterStanding::Blocked : GroupInviteGate::InviterStanding::Contact;
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<InMemoryGroupRosterStore> roster_;
  std::map<std::string, GroupInviteGate::InviterStanding> standings_;
  std::unique_ptr<GroupInviteGate> gate_;
};

TEST_F(GroupInviteGateTest, ContactsOnlyAllowsKnownInviter) {
  gate_->SetInboundPolicy(GroupInvitePolicy::ContactsOnly);
  AddContact("account:alice");
  auto allowed = gate_->AllowsInboundInvite(MakeInvite());
  ASSERT_TRUE(allowed);
  EXPECT_TRUE(*allowed);
}

TEST_F(GroupInviteGateTest, ContactsOnlyRejectsUnknownInviter) {
  gate_->SetInboundPolicy(GroupInvitePolicy::ContactsOnly);
  auto allowed = gate_->AllowsInboundInvite(MakeInvite("account:stranger"));
  ASSERT_TRUE(allowed);
  EXPECT_FALSE(*allowed);
}

TEST_F(GroupInviteGateTest, NobodyRejectsAll) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Nobody);
  AddContact("account:alice");
  auto allowed = gate_->AllowsInboundInvite(MakeInvite());
  ASSERT_TRUE(allowed);
  EXPECT_FALSE(*allowed);
}

TEST_F(GroupInviteGateTest, EveryoneAllowsUnknown) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Everyone);
  auto allowed = gate_->AllowsInboundInvite(MakeInvite("account:stranger"));
  ASSERT_TRUE(allowed);
  EXPECT_TRUE(*allowed);
}

TEST_F(GroupInviteGateTest, BlockedContactRejected) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Everyone);
  AddContact("account:alice", /*blocked=*/true);
  auto allowed = gate_->AllowsInboundInvite(MakeInvite());
  ASSERT_TRUE(allowed);
  EXPECT_FALSE(*allowed);
}

TEST_F(GroupInviteGateTest, ExpiredInviteRejected) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Everyone);
  auto invite = MakeInvite();
  invite.expires_at = util::NowUnixMs() - 1000;
  auto allowed = gate_->AllowsInboundInvite(invite);
  ASSERT_TRUE(allowed);
  EXPECT_FALSE(*allowed);
}

TEST_F(GroupInviteGateTest, RateLimitRejectsAboveCap) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Everyone);
  for (size_t i = 0; i < GroupInviteGate::kMaxPendingInvitesPerDay; ++i) {
    PendingGroupInvite pending;
    pending.invite_nonce = "nonce-" + std::to_string(i);
    pending.group_id = "group:x";
    pending.inviter_identity = "account:alice";
    pending.invitee_identity = "account:bob";
    pending.status = InviteStatus::Pending;
    pending.created_at = util::NowUnixMs();
    ASSERT_TRUE(roster_->UpsertPendingInvite(pending));
  }
  auto allowed = gate_->AllowsInboundInvite(MakeInvite());
  ASSERT_TRUE(allowed);
  EXPECT_FALSE(*allowed);
}

TEST_F(GroupInviteGateTest, OurOwnOutgoingInvitesDoNotFillTheInboundCap) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Everyone);
  for (size_t i = 0; i < GroupInviteGate::kMaxPendingInvitesPerDay + 5; ++i) {
    PendingGroupInvite pending;
    pending.invite_nonce = "out-" + std::to_string(i);
    pending.group_id = "group:x";
    pending.inviter_identity = "account:me";
    pending.invitee_identity = "account:bob" + std::to_string(i);
    pending.status = InviteStatus::Pending;
    pending.created_at = util::NowUnixMs();
    pending.outbound = true;
    ASSERT_TRUE(roster_->UpsertPendingInvite(pending));
  }
  auto allowed = gate_->AllowsInboundInvite(MakeInvite());
  ASSERT_TRUE(allowed);
  EXPECT_TRUE(*allowed);
}

/** G022: an invite Alice made during the call `call_id`. */
GroupInvitePayload MakeCallInvite(const std::string& call_id, const std::string& inviter = "account:alice") {
  GroupInvitePayload invite = MakeInvite(inviter);
  invite.context = kGroupInviteContextCall;
  invite.call_id = call_id;
  return invite;
}

class GroupInviteGateInCallTest : public GroupInviteGateTest {
protected:
  void SetUp() override {
    GroupInviteGateTest::SetUp();
    gate_->SetInboundPolicy(GroupInvitePolicy::ContactsOnly);
    // The calls say: this device is in call:live, with Alice in it (and nobody else is in a call with us).
    gate_->SetInLiveCall([this](const std::string& call_id, const std::string& inviter) {
      ++asked_;
      return call_id == "call:live" && inviter == "account:alice";
    });
  }

  bool Allows(const GroupInvitePayload& invite) {
    auto allowed = gate_->AllowsInboundInvite(invite);
    EXPECT_TRUE(allowed);
    return allowed && *allowed;
  }

  int asked_ = 0;
};

TEST_F(GroupInviteGateInCallTest, AStrangerInTheLiveCallWithUsMayInviteUsFromIt) {
  EXPECT_TRUE(Allows(MakeCallInvite("call:live"))) << "met in the call: not a stranger for this invite";
}

TEST_F(GroupInviteGateInCallTest, AnotherCallOrNoneIsNoReason) {
  EXPECT_FALSE(Allows(MakeCallInvite("call:other"))) << "a call we are not in with them";
  EXPECT_FALSE(Allows(MakeCallInvite("call:live", "account:mallory"))) << "the inviter is not in our call";
  GroupInvitePayload no_call_id = MakeCallInvite("");
  EXPECT_FALSE(Allows(no_call_id)) << "context call without a call id";
  GroupInvitePayload no_context = MakeInvite();
  no_context.call_id = "call:live";
  EXPECT_FALSE(Allows(no_context)) << "a call id without context call is an ordinary invite";
  EXPECT_FALSE(Allows(MakeInvite())) << "an ordinary invite from a stranger";
}

TEST_F(GroupInviteGateInCallTest, ABlockedInviterStaysRefusedInACall) {
  AddContact("account:alice", /*blocked=*/true);
  EXPECT_FALSE(Allows(MakeCallInvite("call:live")));
  EXPECT_EQ(asked_, 0) << "blocked is decided before the call is asked about";
}

TEST_F(GroupInviteGateInCallTest, NobodyStaysNobody) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Nobody);
  EXPECT_FALSE(Allows(MakeCallInvite("call:live")));
}

TEST_F(GroupInviteGateInCallTest, AContactNeedsNoCall) {
  AddContact("account:carol");
  EXPECT_TRUE(Allows(MakeCallInvite("call:other", "account:carol")));
}

TEST_F(GroupInviteGateInCallTest, WithoutTheCallsPortACallIsNoReason) {
  gate_->SetInLiveCall({});
  EXPECT_FALSE(Allows(MakeCallInvite("call:live")));
}

TEST_F(GroupInviteGateInCallTest, TheCallShowsItsInvitesUntilAnswered) {
  const GroupInvitePayload first = MakeCallInvite("call:live");
  GroupInvitePayload second = MakeCallInvite("call:live", "account:carol");
  GroupInvitePayload elsewhere = MakeCallInvite("call:other");
  GroupInvitePayload expired = MakeCallInvite("call:live");
  expired.expires_at = util::NowUnixMs() - 1;
  gate_->NoteInviteInCall(first);
  gate_->NoteInviteInCall(second);
  gate_->NoteInviteInCall(elsewhere);
  gate_->NoteInviteInCall(expired);
  gate_->NoteInviteInCall(MakeInvite()); // not made in a call: not noted

  auto live = gate_->InvitesInCall("call:live", util::NowUnixMs());
  ASSERT_EQ(live.size(), 2u) << "this call's, not expired";
  EXPECT_EQ(live[0].invite_nonce, first.invite_nonce) << "oldest first";
  EXPECT_EQ(live[0].group_title, "Test");
  EXPECT_EQ(live[0].inviter_identity, "account:alice");
  EXPECT_EQ(live[1].invite_nonce, second.invite_nonce);
  EXPECT_EQ(gate_->CallOfInvite(first.invite_nonce), "call:live");

  gate_->ForgetInviteInCall(first.invite_nonce);
  live = gate_->InvitesInCall("call:live", util::NowUnixMs());
  ASSERT_EQ(live.size(), 1u);
  EXPECT_EQ(live[0].invite_nonce, second.invite_nonce);
  EXPECT_EQ(gate_->CallOfInvite(first.invite_nonce), "");
  EXPECT_TRUE(gate_->InvitesInCall("", util::NowUnixMs()).empty());
}

} // namespace
} // namespace pbr
