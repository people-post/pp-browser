#include "domain/delivery/PeerRouteBook.h"

#include "common/chat/RelayEnvelope.h"
#include "common/thread/ThreadTypes.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "lib/people/ContactsStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>

namespace pbr {
namespace {

Thread DirectThreadWith(const std::string& account) {
  Thread thread;
  thread.id = "t1";
  thread.kind = ThreadKind::Direct;
  thread.peer_identity_value = account;
  return thread;
}

/** A directory that knows one account and counts the lookups made. */
class OneAccountDirectory final : public mesh::IDirectoryClient {
public:
  OneAccountDirectory(std::string account, std::string relay)
      : account_(std::move(account)), relay_(std::move(relay)) {}
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override {
    return std::vector<mesh::DirectoryHit>{};
  }
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string&) override { return Error("unused"); }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override {
    ++lookups;
    if (account_id != account_) {
      return Error("Account not found");
    }
    return Hit();
  }
  mesh::DirectoryHit Hit() const {
    mesh::DirectoryHit hit;
    hit.hit_id = relay_;
    hit.ids = {{mesh::ContactIdKind::Account, account_, true}, {mesh::ContactIdKind::RelayUser, relay_, true}};
    return hit;
  }
  int lookups = 0;

private:
  std::string account_;
  std::string relay_;
};

class PeerRouteBookTest : public ::testing::Test {
protected:
  PeerRouteBookTest()
      : dir_(std::filesystem::temp_directory_path() / ("pp_route_book_" + std::to_string(std::random_device{}()))),
        contacts_((std::filesystem::create_directories(dir_), dir_.string())) {}
  ~PeerRouteBookTest() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
  ContactsStore contacts_;
};

RelayEnvelope FromAccount(const std::string& account, const std::string& relay) {
  RelayEnvelope envelope;
  envelope.sender_contact_id = account;
  envelope.sender_relay_id = relay;
  return envelope;
}

TEST_F(PeerRouteBookTest, LearnsARouteOnlyFromAVerifiedEnvelope) {
  PeerRouteBook routes(contacts_);
  const Thread thread = DirectThreadWith("account:alice");

  routes.RememberFromEnvelope(FromAccount("account:alice", "relay:forged"), /*sender_verified=*/false);
  EXPECT_FALSE(routes.Resolve(thread).has_value()) << "an unverified envelope's relay id is anyone's claim";

  routes.RememberFromEnvelope(FromAccount("account:alice", "relay:alice"), /*sender_verified=*/true);
  EXPECT_EQ(routes.Resolve(thread), std::optional<std::string>("relay:alice"));
}

TEST_F(PeerRouteBookTest, IgnoresValuesThatAreNotAnAccountAndARelayId) {
  PeerRouteBook routes(contacts_);
  routes.NoteAccountRoute("alice", "relay:alice");
  routes.NoteAccountRoute("account:bob", "bob");
  EXPECT_FALSE(routes.Resolve(DirectThreadWith("alice")).has_value());
  EXPECT_FALSE(routes.Resolve(DirectThreadWith("account:bob")).has_value());
}

TEST_F(PeerRouteBookTest, WithoutADirectoryAnUnknownPeerHasNoRoute) {
  PeerRouteBook routes(contacts_);
  EXPECT_FALSE(routes.ResolveWithDirectory(DirectThreadWith("account:carol")).has_value());
}

Contact ContactWithRoute(const std::string& account, const std::string& relay) {
  Contact contact;
  contact.id = "contact-" + account;
  contact.display_name = "Peer";
  contact.ids = {{mesh::ContactIdKind::Account, account, true}, {mesh::ContactIdKind::RelayUser, relay, true}};
  return contact;
}

TEST_F(PeerRouteBookTest, ForgetDropsEveryCachedRouteAndTheDirectoryAnswersNext) {
  ASSERT_TRUE(static_cast<bool>(contacts_.Upsert(ContactWithRoute("account:alice", "relay:old"))));
  OneAccountDirectory directory("account:alice", "relay:new");
  DirectoryShadowCache shadows(directory);
  PeerRouteBook routes(contacts_);
  routes.SetSources(&shadows, &directory);
  const Thread thread = DirectThreadWith("account:alice");
  routes.NoteAccountRoute("account:alice", "relay:learned");
  EXPECT_EQ(routes.Resolve(thread), std::optional<std::string>("relay:old")) << "the contact's route comes first";

  routes.Forget("account:alice");
  EXPECT_FALSE(routes.Resolve(thread).has_value()) << "the contact's stored id is no longer trusted";
  EXPECT_EQ(directory.lookups, 0);

  EXPECT_EQ(routes.ResolveWithDirectory(thread), std::optional<std::string>("relay:new"));
  EXPECT_EQ(directory.lookups, 1);
  EXPECT_EQ(routes.Resolve(thread), std::optional<std::string>("relay:new")) << "the directory's answer now wins";
}

TEST_F(PeerRouteBookTest, ForgetLeavesOtherAccountsAlone) {
  ASSERT_TRUE(static_cast<bool>(contacts_.Upsert(ContactWithRoute("account:alice", "relay:alice"))));
  ASSERT_TRUE(static_cast<bool>(contacts_.Upsert(ContactWithRoute("account:bob", "relay:bob"))));
  PeerRouteBook routes(contacts_);
  routes.Forget("account:alice");
  EXPECT_FALSE(routes.Resolve(DirectThreadWith("account:alice")).has_value());
  EXPECT_EQ(routes.Resolve(DirectThreadWith("account:bob")), std::optional<std::string>("relay:bob"));
}

TEST_F(PeerRouteBookTest, ForgetAllDropsLearnedContactAndDirectoryCacheRoutes) {
  ASSERT_TRUE(static_cast<bool>(contacts_.Upsert(ContactWithRoute("account:alice", "relay:old"))));
  OneAccountDirectory directory("account:bob", "relay:bob");
  DirectoryShadowCache shadows(directory);
  shadows.Put(directory.Hit());
  PeerRouteBook routes(contacts_);
  routes.SetSources(&shadows, &directory);
  routes.NoteAccountRoute("account:carol", "relay:carol");
  ASSERT_EQ(routes.Resolve(DirectThreadWith("account:bob")), std::optional<std::string>("relay:bob"));

  routes.ForgetAll();
  EXPECT_FALSE(routes.Resolve(DirectThreadWith("account:alice")).has_value());
  EXPECT_FALSE(routes.Resolve(DirectThreadWith("account:bob")).has_value());
  EXPECT_FALSE(routes.Resolve(DirectThreadWith("account:carol")).has_value());

  routes.RememberFromEnvelope(FromAccount("account:alice", "relay:alice-new"), /*sender_verified=*/true);
  EXPECT_EQ(routes.Resolve(DirectThreadWith("account:alice")), std::optional<std::string>("relay:alice-new"))
      << "a route learned after the switch is used";
}

} // namespace
} // namespace pbr
