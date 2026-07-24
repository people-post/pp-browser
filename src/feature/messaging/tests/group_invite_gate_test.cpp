#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/GroupTypes.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/people/ContactsStore.h"
#include "feature/messaging/GroupInviteGate.h"

#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace pbr {
namespace {

GroupInvitePayload MakeInvite(const std::string& inviter = "relay:alice") {
  GroupInvitePayload invite;
  invite.group_id = "group:test";
  invite.group_title = "Test";
  invite.inviter_identity = inviter;
  invite.invitee_identity = "relay:bob";
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
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    roster_ = std::make_unique<GroupRosterStore>(store_->ProfileDbPath());
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    gate_ = std::make_unique<GroupInviteGate>(*contacts_, *roster_);
  }

  void TearDown() override {
    gate_.reset();
    contacts_.reset();
    roster_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  void AddContact(const std::string& relay_id, const TrustLevel trust = TrustLevel::Friendly) {
    Contact contact;
    contact.id = util::GenerateUuid();
    contact.display_name = relay_id;
    contact.trust = trust;
    ContactId id;
    id.kind = ContactIdKind::RelayUser;
    id.value = relay_id;
    id.primary = true;
    contact.ids.push_back(id);
    ASSERT_TRUE(contacts_->Upsert(contact));
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<GroupRosterStore> roster_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<GroupInviteGate> gate_;
};

TEST_F(GroupInviteGateTest, ContactsOnlyAllowsKnownInviter) {
  gate_->SetInboundPolicy(GroupInvitePolicy::ContactsOnly);
  AddContact("relay:alice");
  auto allowed = gate_->AllowsInboundInvite(MakeInvite());
  ASSERT_TRUE(allowed);
  EXPECT_TRUE(*allowed);
}

TEST_F(GroupInviteGateTest, ContactsOnlyRejectsUnknownInviter) {
  gate_->SetInboundPolicy(GroupInvitePolicy::ContactsOnly);
  auto allowed = gate_->AllowsInboundInvite(MakeInvite("relay:stranger"));
  ASSERT_TRUE(allowed);
  EXPECT_FALSE(*allowed);
}

TEST_F(GroupInviteGateTest, NobodyRejectsAll) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Nobody);
  AddContact("relay:alice");
  auto allowed = gate_->AllowsInboundInvite(MakeInvite());
  ASSERT_TRUE(allowed);
  EXPECT_FALSE(*allowed);
}

TEST_F(GroupInviteGateTest, EveryoneAllowsUnknown) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Everyone);
  auto allowed = gate_->AllowsInboundInvite(MakeInvite("relay:stranger"));
  ASSERT_TRUE(allowed);
  EXPECT_TRUE(*allowed);
}

TEST_F(GroupInviteGateTest, BlockedContactRejected) {
  gate_->SetInboundPolicy(GroupInvitePolicy::Everyone);
  AddContact("relay:alice", TrustLevel::Blocked);
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
    pending.inviter_identity = "relay:alice";
    pending.invitee_identity = "relay:bob";
    pending.status = InviteStatus::Pending;
    pending.created_at = util::NowUnixMs();
    ASSERT_TRUE(roster_->UpsertPendingInvite(pending));
  }
  auto allowed = gate_->AllowsInboundInvite(MakeInvite());
  ASSERT_TRUE(allowed);
  EXPECT_FALSE(*allowed);
}

} // namespace
} // namespace pbr
