#include "lib/people/ContactReachability.h"

#include "lib/people/ContactTypes.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

Contact MakePeerOnlyContact(const std::string& peer_id) {
  Contact c;
  c.id = "c1";
  c.ids.push_back({mesh::ContactIdKind::PeerId, peer_id, true});
  return c;
}

} // namespace

TEST(ContactReachabilityTest, PeerIdOnlyNotReachableWithoutMultiaddr) {
  const Contact c = MakePeerOnlyContact("12D3KooWTest");
  EXPECT_FALSE(IsContactReachableForMessaging(c, true));
}

TEST(ContactReachabilityTest, RelayContactReachableWhenRelayConfigured) {
  Contact c;
  c.ids.push_back({mesh::ContactIdKind::RelayUser, "relay:alice", true});
  EXPECT_TRUE(IsContactReachableForMessaging(c, true));
  EXPECT_FALSE(IsContactReachableForMessaging(c, false));
}

TEST(ContactReachabilityTest, AnAccountWithKnownKeysIsReachable) {
  // #409: a contact added from a QR code has its Account ID and device ids, no relay id or address.
  Contact c = MakePeerOnlyContact("12D3KooWTest");
  c.ids.push_back({mesh::ContactIdKind::Account, "account:alice", true});
  EXPECT_FALSE(IsContactReachableForMessaging(c, true)) << "no card yet: nothing to encrypt to";
  EXPECT_TRUE(IsContactReachableForMessaging(c, true, /*account_keys_known=*/true));
  EXPECT_FALSE(IsContactReachableForMessaging(MakePeerOnlyContact("12D3KooWTest"), true, true))
      << "keys of an account the contact does not name prove nothing";
  c.trust = TrustLevel::Blocked;
  EXPECT_FALSE(IsContactReachableForMessaging(c, true, true));
}

TEST(ContactReachabilityTest, PastedMultiaddrReachable) {
  Contact c = MakePeerOnlyContact("12D3KooWTest");
  c.multiaddrs.push_back("/ip4/192.168.1.2/udp/1234/adp/1.0.0/p2p/12D3KooWTest");
  EXPECT_TRUE(IsContactReachableForMessaging(c, false));
}

} // namespace pbr
