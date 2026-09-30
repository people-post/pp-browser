#include "domain/people/ContactAddressDisclosure.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

Contact MakeContact(const std::string& peer_id, const std::string& account_id, const TrustLevel trust) {
  Contact c;
  c.id = "c-" + peer_id;
  c.ids.push_back({ContactIdKind::PeerId, peer_id, true});
  c.ids.push_back({ContactIdKind::Account, account_id, false});
  c.trust = trust;
  c.local.trust = trust;
  return c;
}

// projects/privacy T1: the address book decides who may learn our address.
TEST(ContactAddressDisclosureTest, ContactsFriendlyAndBlockedByTrust) {
  const std::vector<Contact> book = {
      MakeContact("12D3KooWPlain", "account:plain", TrustLevel::Unknown),
      MakeContact("12D3KooWFriend", "account:friend", TrustLevel::Friendly),
      MakeContact("12D3KooWBlocked", "account:blocked", TrustLevel::Blocked),
  };

  const auto contacts = BuildAddressDisclosurePolicy(DirectAudience::Contacts, book);
  EXPECT_TRUE(contacts.AllowsDirect("12D3KooWPlain"));
  EXPECT_TRUE(contacts.AllowsDirect("account:plain"));
  EXPECT_TRUE(contacts.AllowsDirect("12D3KooWFriend"));
  EXPECT_FALSE(contacts.AllowsDirect("12D3KooWBlocked")) << "a Blocked contact is not a contact here";
  EXPECT_FALSE(contacts.AllowsDirect("account:blocked"));
  EXPECT_FALSE(contacts.AllowsDirect("12D3KooWStranger"));

  const auto friendly = BuildAddressDisclosurePolicy(DirectAudience::Friendly, book);
  EXPECT_TRUE(friendly.AllowsDirect("12D3KooWFriend"));
  EXPECT_TRUE(friendly.AllowsDirect("account:friend"));
  EXPECT_FALSE(friendly.AllowsDirect("12D3KooWPlain"));

  const auto everyone = BuildAddressDisclosurePolicy(DirectAudience::Everyone, book);
  EXPECT_TRUE(everyone.AllowsDirect("12D3KooWStranger"));
  EXPECT_FALSE(everyone.AllowsDirect("12D3KooWBlocked"));
}

} // namespace
} // namespace pbr
