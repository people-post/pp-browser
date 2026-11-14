#include "lib/messaging/ContactLocator.h"

#include "foundation/crypto/CryptoUtil.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

TEST(ContactLocatorTest, RoundTripsAndIsFoundInsideAnInviteLine) {
  const ContactLocator locator{.account_id = "account:abc", .nickname = "Bob", .devices = {"QmPhone", "QmLaptop"}};
  const std::string encoded = EncodeContactLocator(locator);
  EXPECT_EQ(encoded.rfind(kContactLocatorPrefix, 0), 0u);
  EXPECT_EQ(encoded.find_first_of("+/= \n"), std::string::npos) << "url-safe, one token";
  auto found = FindContactLocator("Hi! Add me on PP: " + encoded + "\nsee you");
  ASSERT_TRUE(found) << found.error().message;
  EXPECT_EQ(*found, locator);
  EXPECT_LT(encoded.size(), 400u) << "fits a QR code comfortably";
}

TEST(ContactLocatorTest, ALocatorMustNameAnAccount) {
  EXPECT_FALSE(FindContactLocator("no locator here"));
  EXPECT_FALSE(FindContactLocator(std::string(kContactLocatorPrefix) + "!!!"));
  const ContactLocator not_an_account{.account_id = "peer:abc"};
  EXPECT_FALSE(FindContactLocator(EncodeContactLocator(not_an_account)));
}

TEST(ContactLocatorTest, AFriendsLocatorCarriesHisOwnNicknameNotTheRemarkAndHisDevices) {
  Contact friend_contact;
  friend_contact.id = "c-1";
  friend_contact.local.display_name = "Mom (private remark)";
  friend_contact.remote.nickname = "Alice";
  friend_contact.remote.ids = {{mesh::ContactIdKind::Account, "account:abc", true},
                               {mesh::ContactIdKind::PeerId, "QmPhone", true},
                               {mesh::ContactIdKind::PeerId, "QmLaptop", false}};
  SyncContactMirrors(friend_contact);
  const std::string encoded = ContactShareLocator(friend_contact);
  auto found = FindContactLocator(encoded);
  ASSERT_TRUE(found) << found.error().message;
  EXPECT_EQ(found->account_id, "account:abc");
  EXPECT_EQ(found->nickname, "Alice");
  EXPECT_EQ(found->devices, (std::vector<std::string>{"QmPhone", "QmLaptop"}));
  EXPECT_EQ(encoded.find("Mom"), std::string::npos);
}

TEST(ContactLocatorTest, AContactWithoutAnAccountIdHasNoLocator) {
  Contact devices_only;
  devices_only.remote.ids = {{mesh::ContactIdKind::PeerId, "QmPhone", true}};
  SyncContactMirrors(devices_only);
  EXPECT_TRUE(ContactShareLocator(devices_only).empty());
}

TEST(Base64UrlTest, RoundTripsWithoutPadding) {
  const ByteVector bytes{0xfb, 0xff, 0x00, 0x10};
  const std::string encoded = Base64UrlEncode(bytes);
  EXPECT_EQ(encoded, "-_8AEA");
  EXPECT_EQ(Base64UrlDecode(encoded).value(), bytes);
  EXPECT_FALSE(Base64UrlDecode("not base64!"));
}

} // namespace
} // namespace pbr
