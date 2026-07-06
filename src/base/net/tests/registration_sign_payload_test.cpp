#include "base/crypto/CryptoUtil.h"
#include "base/crypto/HybridKem.h"
#include "base/net/RegistrationSignPayload.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

constexpr const char kVectorPublicKeyB64[] = "QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkI=";

std::string VectorKemPublicKeyB64(uint8_t fill = 0x43) {
  return Base64Encode(std::vector<uint8_t>(kHybridKemPublicKeyBytes, fill));
}

} // namespace

TEST(RegistrationSignPayloadTest, BuildRegistrationSignBytesBindsKemKey) {
  const auto bytes = BuildRegistrationSignBytes("ch-1", kVectorPublicKeyB64, VectorKemPublicKeyB64(), "ed25519",
                                                1700000000000);
  ASSERT_EQ(bytes.size(), 1299u);
  EXPECT_EQ(bytes[bytes.size() - 10], 0x43);
  EXPECT_EQ(bytes[bytes.size() - 9], 0x00);
  EXPECT_EQ(bytes[bytes.size() - 8], 0x00);
  EXPECT_EQ(bytes[bytes.size() - 1], 0x00);

  const auto other_bytes = BuildRegistrationSignBytes("ch-1", kVectorPublicKeyB64, VectorKemPublicKeyB64(0x44),
                                                      "ed25519", 1700000000000);
  EXPECT_NE(bytes, other_bytes);
}

TEST(RegistrationSignPayloadTest, BuildProfileUpdateSignBytesMatchesGoldenVector) {
  const auto bytes = BuildProfileUpdateSignBytes("relay:u1", "alice", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d70726f66696c652d76310001000000000000000872656c61793a"
            "75310000000000000005616c6963650000018bcfe56803");
}

} // namespace pbr
