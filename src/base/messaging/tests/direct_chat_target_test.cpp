#include "base/people/DirectChatTargetFromContact.h"
#include "common/chat/MessagingJson.h"
#include "base/people/ContactTypes.h"

#include <gtest/gtest.h>

namespace {

using namespace pbr;

TEST(DirectChatTargetTest, UsesAccountId) {
  Contact contact;
  contact.ids = {{ContactIdKind::Account, "account:alice", true},
                 {ContactIdKind::RelayUser, "relay:alice", false},
                 {ContactIdKind::PeerId, "12D3KooWPeer", false}};

  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  EXPECT_EQ(target.peer_identity_kind, "account");
  EXPECT_EQ(target.peer_identity_value, "account:alice");
}

TEST(DirectChatTargetTest, EmptyWithoutAccountEvenIfRelayOrPeerPresent) {
  Contact contact;
  contact.ids = {{ContactIdKind::RelayUser, "relay:alice", true},
                 {ContactIdKind::PeerId, "12D3KooWPeer", false}};

  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  EXPECT_TRUE(target.peer_identity_value.empty());
}

TEST(DirectChatTargetTest, EmptyWhenNoIdentity) {
  Contact contact;
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  EXPECT_TRUE(target.peer_identity_value.empty());
}

} // namespace
