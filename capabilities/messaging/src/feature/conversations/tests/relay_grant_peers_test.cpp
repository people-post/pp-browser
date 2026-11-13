#include "feature/conversations/RelayGrantPeers.h"

#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace pbr {
namespace {

// Multi-relay MR005: who gets a relay grant — every contact not blocked, and every member of a group we are in (members
// are not always contacts, and group mail must not be throttled as a stranger's); never a blocked contact, never us.

class RelayGrantPeersTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_relay_grant_peers_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    peers_ = std::make_unique<RelayGrantPeers>(*contacts_, rosters_, [] { return std::string("account:me"); });
  }
  void TearDown() override {
    peers_.reset();
    contacts_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  void AddContact(const std::string& id, const std::string& account, const TrustLevel trust) {
    Contact contact;
    contact.id = id;
    contact.remote.ids = {{mesh::ContactIdKind::Account, account, true}};
    contact.local.trust = trust;
    SyncContactMirrors(contact);
    ASSERT_TRUE(contacts_->Upsert(contact));
  }
  void AddGroup(const std::string& group_id, const std::vector<std::string>& members,
                const GroupSyncState state = GroupSyncState::Active) {
    GroupMetadata metadata;
    metadata.group_id = group_id;
    metadata.owner_identity = members.front();
    metadata.sync_state = state;
    ASSERT_TRUE(rosters_.UpsertMetadata(metadata));
    for (const std::string& member : members) {
      ASSERT_TRUE(rosters_.UpsertMember(group_id, GroupRosterMember{.member_identity = member}));
    }
  }

  std::filesystem::path dir_;
  std::unique_ptr<ContactsStore> contacts_;
  InMemoryDeliveryStore threads_;
  InMemoryGroupRosterStore rosters_{threads_};
  std::unique_ptr<RelayGrantPeers> peers_;
};

TEST_F(RelayGrantPeersTest, ContactsAndCoMembersAreGrantedBlockedOnesAndWeAreNot) {
  AddContact("c-ann", "account:ann", TrustLevel::Friendly);
  AddContact("c-bob", "account:bob", TrustLevel::Unknown);
  AddContact("c-mal", "account:mallory", TrustLevel::Blocked);
  AddGroup("g-hike", {"account:me", "account:carol", "account:mallory"});
  AddGroup("g-gone", {"account:me", "account:dave"}, GroupSyncState::Removed);

  EXPECT_TRUE(peers_->Granted("account:ann"));
  EXPECT_TRUE(peers_->Granted("account:bob")) << "an added contact, trusted or not";
  EXPECT_TRUE(peers_->Granted("account:carol")) << "a co-member who is no contact";
  EXPECT_FALSE(peers_->Granted("account:mallory")) << "blocked, in a group or not";
  EXPECT_FALSE(peers_->Granted("account:dave")) << "a group we were removed from";
  EXPECT_FALSE(peers_->Granted("account:me"));
  EXPECT_FALSE(peers_->Granted("account:stranger"));
  EXPECT_EQ(peers_->All(), (std::vector<std::string>{"account:ann", "account:bob", "account:carol"}));
  EXPECT_EQ(peers_->Blocked(), std::vector<std::string>{"account:mallory"});
}

} // namespace
} // namespace pbr
