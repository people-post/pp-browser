#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/net/RegistrationSignPayload.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

std::string VectorMlDsaPublicKeyB64(uint8_t fill = 0x42) {
  return Base64Encode(std::vector<uint8_t>(kMlDsa65PublicKeyBytes, fill));
}

std::string VectorKemPublicKeyB64(uint8_t fill = 0x43) {
  return Base64Encode(std::vector<uint8_t>(kHybridKemPublicKeyBytes, fill));
}

} // namespace

TEST(RegistrationSignPayloadTest, BuildRegistrationSignBytesBindsKemKey) {
  const auto bytes = BuildRegistrationSignBytes("ch-1", VectorMlDsaPublicKeyB64(), VectorKemPublicKeyB64(),
                                                "ml-dsa-65", 1700000000000);
  // domain+NUL + ver + len-prefixed "ch-1" + ML-DSA-65 pk + ML-KEM-768 pk + alg + i64 ts
  ASSERT_EQ(bytes.size(), kMlDsa65PublicKeyBytes + kHybridKemPublicKeyBytes + 51u);
  EXPECT_EQ(bytes[bytes.size() - 10], 0x43);
  EXPECT_EQ(bytes[bytes.size() - 9], 0x01); // ml-dsa-65 wire alg
  EXPECT_EQ(bytes[bytes.size() - 8], 0x00);
  EXPECT_EQ(bytes[bytes.size() - 1], 0x00);

  const auto other_bytes = BuildRegistrationSignBytes("ch-1", VectorMlDsaPublicKeyB64(), VectorKemPublicKeyB64(0x44),
                                                      "ml-dsa-65", 1700000000000);
  EXPECT_NE(bytes, other_bytes);
}

// Same vector as app-static-api RelayApiAuthVerifier.test.ts ("buildAccountRevokeSignBytes matches ...").
TEST(RegistrationSignPayloadTest, BuildAccountRevokeSignBytesMatchesGoldenVector) {
  const auto bytes = BuildAccountRevokeSignBytes("relay:u1", 1700000000005);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d7265766f6b652d76310001000000000000000872656c61793a7531"
            "0000018bcfe56805");
}

TEST(RegistrationSignPayloadTest, BuildProfileUpdateSignBytesMatchesGoldenVector) {
  const auto bytes = BuildProfileUpdateSignBytes("relay:u1", "alice", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d70726f66696c652d76310001000000000000000872656c61793a"
            "75310000000000000005616c6963650000018bcfe56803");
}

// Layout mirrors the relay server's buildMeshHeartbeatSignBytes.
TEST(RegistrationSignPayloadTest, BuildRelayHeartbeatSignBytesMatchesGoldenVector) {
  const auto bytes = BuildRelayHeartbeatSignBytes("relay:u1", "12D3KooWpeer", "active", 1700000000007);
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6865617274626561742d76310001000000000000000872656c61793a"
            "7531000000000000000c313244334b6f6f577065657200000000000000066163746976650000018bcfe56807");
}

TEST(RegistrationSignPayloadTest, HeartbeatSignBytesBindEveryField) {
  const auto base = BuildRelayHeartbeatSignBytes("relay:u1", "p1", "active", 1);
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes("relay:u2", "p1", "active", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes("relay:u1", "p2", "active", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes("relay:u1", "p1", "draining", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes("relay:u1", "p1", "active", 2));
}

} // namespace pbr
