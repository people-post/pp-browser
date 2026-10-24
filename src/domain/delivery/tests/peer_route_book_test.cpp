#include "domain/delivery/PeerRouteBook.h"

#include "common/chat/RelayEnvelope.h"
#include "lib/messaging/RelayReceivePipeline.h"
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

class PeerRouteBookTest : public ::testing::Test {
protected:
  PeerRouteBookTest()
      : dir_(std::filesystem::temp_directory_path() /
             ("pp_route_book_" + std::to_string(std::random_device{}()))),
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

  RelayReceiveOutcome unverified;
  routes.RememberFromEnvelope(FromAccount("account:alice", "relay:forged"), unverified);
  EXPECT_FALSE(routes.Resolve(thread).has_value()) << "an unverified envelope's relay id is anyone's claim";

  RelayReceiveOutcome verified;
  verified.sender_verified = true;
  routes.RememberFromEnvelope(FromAccount("account:alice", "relay:alice"), verified);
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

} // namespace
} // namespace pbr
