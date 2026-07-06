#include "base/crypto/CryptoUtil.h"
#include "base/net/RegistrationSignPayload.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr const char kVectorPublicKeyB64[] = "QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkI=";

} // namespace

TEST(RegistrationSignPayloadTest, BuildRegistrationSignBytesMatchesGoldenVector) {
  const auto bytes =
      BuildRegistrationSignBytes("ch-1", kVectorPublicKeyB64, "ed25519", 1700000000000);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d72656769737465722d76310001000000000000000463682d"
            "314242424242424242424242424242424242424242424242424242424242424242000000018bcfe56800");
}

TEST(RegistrationSignPayloadTest, BuildProfileUpdateSignBytesMatchesGoldenVector) {
  const auto bytes = BuildProfileUpdateSignBytes("relay:u1", "alice", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d70726f66696c652d76310001000000000000000872656c61793a"
            "75310000000000000005616c6963650000018bcfe56803");
}

} // namespace pbr
