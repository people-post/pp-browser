// The relay id (release-compatibility SIGNED_FORMATS § 3.1, Q5): `relay:` + base64url-unpadded(BLAKE2b-256 of the
// founding operator set, under its domain), one spelling per id. The operator set and its certificate are lib/net's
// RelayCertificate (relay_certificate_test, relay_certificate_vectors.json).

#include "foundation/crypto/RelayId.h"

#include <gtest/gtest.h>

#include <numeric>
#include <string>
#include <vector>

namespace pbr {
namespace {

ByteVector Counting() {
  ByteVector raw(kRelayIdBytes);
  std::iota(raw.begin(), raw.end(), uint8_t{0});
  return raw;
}

TEST(RelayIdTest, WritesRelayPrefixAndBase64UrlUnpadded) {
  const RelayId id = RelayId::FromRaw(Counting()).value();
  EXPECT_EQ(id.ToString(), "relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");
  EXPECT_EQ(id.ToString().size(), kRelayIdTextLength);
  EXPECT_EQ(id.RawBytes(), Counting());
  EXPECT_FALSE(id.empty());
}

TEST(RelayIdTest, ParsesItsOwnSpellingBack) {
  const RelayId id = RelayId::FromRaw(Counting()).value();
  const auto parsed = RelayId::Parse(id.ToString());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, id);
  EXPECT_TRUE(IsRelayIdForm(id.ToString()));
}

TEST(RelayIdTest, RefusesEveryOtherSpelling) {
  const std::string good = "relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
  for (const std::string& bad : std::vector<std::string>{
           std::string(),
           std::string("relay:"),
           good + "=",                                 // padded
           good.substr(0, good.size() - 1),            // 42 characters
           good + "A",                                 // 44 characters
           "Relay:" + good.substr(6),                  // another prefix
           "account:" + good.substr(6),                // an Account ID's prefix
           " " + good,                                 // spaces
           good.substr(0, 10) + "+" + good.substr(11), // standard base64's alphabet
           good.substr(0, 10) + "/" + good.substr(11),
           std::string(
               "relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh9"),      // spare bits set: not the canonical spelling
           std::string("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP"), // a PeerId
       }) {
    EXPECT_FALSE(RelayId::Parse(bad).has_value()) << bad;
    EXPECT_FALSE(IsRelayIdForm(bad)) << bad;
  }
}

TEST(RelayIdTest, RawMustBeThirtyTwoBytes) {
  EXPECT_FALSE(RelayId::FromRaw(ByteVector(31, 1)).has_value());
  EXPECT_FALSE(RelayId::FromRaw(ByteVector(33, 1)).has_value());
  EXPECT_FALSE(RelayId::FromRaw(ByteVector{}).has_value());
  EXPECT_TRUE(RelayId::FromRaw(ByteVector(32, 0)).has_value()) << "all zero is a hash like any other";
}

TEST(RelayIdTest, DefaultIsNoneAndWritesNothing) {
  const RelayId none;
  EXPECT_TRUE(none.empty());
  EXPECT_EQ(none.ToString(), "");
  EXPECT_NE(none, RelayId::FromRaw(ByteVector(32, 0)).value()) << "none is not the all-zero id";
}

// The hash is BLAKE2b-256 over the domain, a NUL, the version byte 1 and the set's encoding, as it is written: the
// expected value is computed outside this code (Python hashlib.blake2b, digest_size=32).
TEST(RelayIdTest, HashesTheDomainVersionAndTheSetsBytes) {
  const ByteVector set = {0x01, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
  const ByteVector input = RelayIdHashInput(set);
  const std::string domain = "pp-browser:relay-id";
  ASSERT_EQ(input.size(), domain.size() + 2 + set.size());
  EXPECT_EQ(std::string(input.begin(), input.begin() + domain.size()), domain);
  EXPECT_EQ(input[domain.size()], 0x00);
  EXPECT_EQ(input[domain.size() + 1], kRelayIdVersion);
  EXPECT_EQ(RelayId::OfOperatorSet(set).ToString(), "relay:yxmTQaDEQTm62hNNUFCmDPVzAHcfjM7hnDAXIqwgB6s");
}

TEST(RelayIdTest, OrdersByBytesForMapsAndSets) {
  const RelayId low = RelayId::FromRaw(ByteVector(32, 0x01)).value();
  const RelayId high = RelayId::FromRaw(ByteVector(32, 0x02)).value();
  EXPECT_LT(low, high);
  EXPECT_LT(RelayId{}, low) << "none sorts first";
}

TEST(RelayIdTest, AnAddressIsAnOriginAndARelayId) {
  const RelayId id = RelayId::FromRaw(Counting()).value();
  const RelayAddress a{.origin = "https://relay.example/api/relay", .relay_id = id};
  RelayAddress b = a;
  EXPECT_EQ(a, b);
  b.relay_id = RelayId{};
  EXPECT_NE(a, b);
}

} // namespace
} // namespace pbr
