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
  const ContactLocator relay_only{.account_id = "relay:abc"};
  EXPECT_FALSE(FindContactLocator(EncodeContactLocator(relay_only)));
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
