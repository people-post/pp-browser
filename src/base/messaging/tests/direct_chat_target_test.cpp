#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/MessagingJson.h"
#include "base/people/ContactTypes.h"

#include <gtest/gtest.h>

namespace {

using namespace pbr;

TEST(DirectChatTargetTest, PrefersRelayOverPeerId) {
  Contact contact;
  contact.ids = {{ContactIdKind::RelayUser, "relay:alice", true},
                 {ContactIdKind::PeerId, "12D3KooWPeer", false}};

  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  EXPECT_EQ(target.peer_identity_kind, "relay_user");
  EXPECT_EQ(target.peer_identity_value, "relay:alice");
}

TEST(DirectChatTargetTest, FallsBackToPeerIdWhenNoRelay) {
  Contact contact;
  contact.ids = {{ContactIdKind::PeerId, "12D3KooWPeer", true}};

  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2e);
  EXPECT_EQ(target.peer_identity_kind, "peer_id");
  EXPECT_EQ(target.peer_identity_value, "12D3KooWPeer");
}

TEST(DirectChatTargetTest, EmptyWhenNoIdentity) {
  Contact contact;
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  EXPECT_TRUE(target.peer_identity_value.empty());
}

} // namespace
