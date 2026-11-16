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

// Multi-relay R4 (MR004), MR008: the locator may carry the preferred relay's address — its origin, within the card's
// bounds, and the PeerId it answers to — so the receiver signs its first send for that PeerId.
constexpr const char kRelayPeer[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";

TEST(ContactLocatorTest, CarriesThePreferredRelaysAddress) {
  const ContactLocator locator{.account_id = "account:abc",
                               .nickname = "Bob",
                               .devices = {"QmPhone"},
                               .relay = {.origin = "https://www.brief.global/api/relay", .peer_id = kRelayPeer}};
  const std::string encoded = EncodeContactLocator(locator);
  auto found = FindContactLocator(encoded);
  ASSERT_TRUE(found) << found.error().message;
  EXPECT_EQ(*found, locator);
  const ContactLocator without{.account_id = "account:abc", .nickname = "Bob", .devices = {"QmPhone"}};
  EXPECT_LE(encoded.size(), EncodeContactLocator(without).size() + 160) << "an origin and a PeerId: about 100 bytes";
}

TEST(ContactLocatorTest, ARelayOutsideTheBoundsOrWithoutItsPeerIdIsLeftOut) {
  const std::string too_long = "https://relay.example/" + std::string(120, 'a');
  for (const AccountCardRelay& relay :
       {AccountCardRelay{.origin = "http://10.0.0.2:8080/api/relay", .peer_id = kRelayPeer},
        AccountCardRelay{.origin = too_long, .peer_id = kRelayPeer},
        AccountCardRelay{.origin = "https://relay.example/?x=1", .peer_id = kRelayPeer},
        AccountCardRelay{.origin = "relay.example", .peer_id = kRelayPeer},
        AccountCardRelay{.origin = "https://relay.example/api/relay"},
        AccountCardRelay{.origin = "https://relay.example/api/relay", .peer_id = "QmNotAPeerId"}}) {
    const std::string encoded = EncodeContactLocator({.account_id = "account:abc", .relay = relay});
    EXPECT_EQ(encoded, EncodeContactLocator({.account_id = "account:abc"})) << relay.origin << " " << relay.peer_id;
  }
  // One written by another app out of bounds, or an origin alone (R4 before MR008): dropped, the locator still holds.
  for (const std::string& relay :
       {std::string(
            R"({"origin":"http://lab.example/api/relay","peer_id":"QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP"})"),
        std::string(R"("https://relay.example/api/relay")")}) {
    const std::string json = R"({"account_id":"account:abc","devices":[],"relay":)" + relay + "}";
    auto found =
        FindContactLocator(std::string(kContactLocatorPrefix) + Base64UrlEncode(ByteVector(json.begin(), json.end())));
    ASSERT_TRUE(found) << found.error().message;
    EXPECT_EQ(found->account_id, "account:abc");
    EXPECT_TRUE(found->relay.origin.empty()) << relay;
  }
}

TEST(ContactLocatorTest, ALocatorWithoutARelayReadsAsBefore) {
  // As an app before R4 wrote it (no `relay`): the same locator, no relay.
  const std::string json = R"({"account_id":"account:abc","nickname":"Bob","devices":["QmPhone"]})";
  auto found =
      FindContactLocator(std::string(kContactLocatorPrefix) + Base64UrlEncode(ByteVector(json.begin(), json.end())));
  ASSERT_TRUE(found) << found.error().message;
  EXPECT_EQ(*found, (ContactLocator{.account_id = "account:abc", .nickname = "Bob", .devices = {"QmPhone"}}));
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
