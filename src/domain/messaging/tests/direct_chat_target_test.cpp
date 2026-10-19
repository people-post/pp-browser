#include "domain/people/DirectChatTargetFromContact.h"
#include "common/chat/MessagingJson.h"
#include "domain/people/ContactTypes.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

namespace {

using namespace pbr;

TEST(DirectChatTargetTest, UsesAccountId) {
  Contact contact;
  contact.ids = {{mesh::ContactIdKind::Account, "account:alice", true},
                 {mesh::ContactIdKind::RelayUser, "relay:alice", false},
                 {mesh::ContactIdKind::PeerId, "12D3KooWPeer", false}};

  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  EXPECT_EQ(target.peer_identity_kind, "account");
  EXPECT_EQ(target.peer_identity_value, "account:alice");
}

TEST(DirectChatTargetTest, EmptyWithoutAccountEvenIfRelayOrPeerPresent) {
  Contact contact;
  contact.ids = {{mesh::ContactIdKind::RelayUser, "relay:alice", true},
                 {mesh::ContactIdKind::PeerId, "12D3KooWPeer", false}};

  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  EXPECT_TRUE(target.peer_identity_value.empty());
}

TEST(DirectChatTargetTest, EmptyWhenNoIdentity) {
  Contact contact;
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  EXPECT_TRUE(target.peer_identity_value.empty());
}

} // namespace
