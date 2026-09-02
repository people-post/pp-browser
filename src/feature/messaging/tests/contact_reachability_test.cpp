#include "feature/messaging/ContactReachability.h"

#include "domain/people/ContactTypes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

Contact MakePeerOnlyContact(const std::string& peer_id) {
  Contact c;
  c.id = "c1";
  c.ids.push_back({ContactIdKind::PeerId, peer_id, true});
  return c;
}

} // namespace

TEST(ContactReachabilityTest, PeerIdOnlyNotReachableWithoutMultiaddr) {
  const Contact c = MakePeerOnlyContact("12D3KooWTest");
  EXPECT_FALSE(IsContactReachableForMessaging(c, true));
}

TEST(ContactReachabilityTest, RelayContactReachableWhenRelayConfigured) {
  Contact c;
  c.ids.push_back({ContactIdKind::RelayUser, "relay:alice", true});
  EXPECT_TRUE(IsContactReachableForMessaging(c, true));
  EXPECT_FALSE(IsContactReachableForMessaging(c, false));
}

TEST(ContactReachabilityTest, PastedMultiaddrReachable) {
  Contact c = MakePeerOnlyContact("12D3KooWTest");
  c.multiaddrs.push_back("/ip4/192.168.1.2/udp/1234/adp/1.0.0/p2p/12D3KooWTest");
  EXPECT_TRUE(IsContactReachableForMessaging(c, false));
}

} // namespace pbr
