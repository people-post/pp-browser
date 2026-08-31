#include "base/messaging/PeerBriefRoute.h"

#include "base/people/ContactsStore.h"

#include <filesystem>
#include <memory>
#include <gtest/gtest.h>

namespace pbr {
namespace {

class PeerBriefRouteTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / "pp_browser_peer_brief_route_test";
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
  }

  void TearDown() override {
    contacts_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<ContactsStore> contacts_;
};

TEST_F(PeerBriefRouteTest, LearnedAccountRouteWithoutContact) {
  Thread thread;
  thread.kind = ThreadKind::Direct;
  thread.peer_identity_kind = "account";
  thread.peer_identity_value = "account:peer";

  std::unordered_map<std::string, std::string> learned;
  EXPECT_FALSE(ResolvePeerBriefRoute(thread, *contacts_, learned).has_value());

  learned["account:peer"] = "relay:peer";
  auto route = ResolvePeerBriefRoute(thread, *contacts_, learned);
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(*route, "relay:peer");
}

TEST_F(PeerBriefRouteTest, ContactRelayPreferredOverLearned) {
  Contact contact;
  contact.id = "c1";
  contact.ids = {{ContactIdKind::Account, "account:peer", true},
                 {ContactIdKind::RelayUser, "relay:from-contact", false}};
  ASSERT_TRUE(contacts_->Upsert(contact));

  Thread thread;
  thread.kind = ThreadKind::Direct;
  thread.peer_identity_kind = "account";
  thread.peer_identity_value = "account:peer";
  thread.participant_contact_ids = {"c1"};

  std::unordered_map<std::string, std::string> learned{{"account:peer", "relay:learned"}};
  auto route = ResolvePeerBriefRoute(thread, *contacts_, learned);
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(*route, "relay:from-contact");
}

TEST_F(PeerBriefRouteTest, DirectoryHitRelay) {
  DirectoryHit hit;
  hit.hit_id = "relay:hit";
  hit.ids = {{ContactIdKind::Account, "account:x", true},
             {ContactIdKind::RelayUser, "relay:from-hit", false}};
  auto route = RelayUserIdFromDirectoryHit(hit);
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(*route, "relay:from-hit");
}

TEST_F(PeerBriefRouteTest, IsRelayUserIdValue) {
  EXPECT_TRUE(IsRelayUserIdValue("relay:abc"));
  EXPECT_FALSE(IsRelayUserIdValue("account:abc"));
  EXPECT_FALSE(IsRelayUserIdValue(""));
}

} // namespace
} // namespace pbr
