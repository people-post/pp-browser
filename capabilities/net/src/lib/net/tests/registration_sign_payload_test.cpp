#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/net/RegistrationSignPayload.h"
#include "lib/net/RelayApiSignPayload.h"

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
  const auto bytes = BuildRegistrationSignBytes("ch-1", VectorMlDsaPublicKeyB64(), VectorKemPublicKeyB64(), "ml-dsa-65",
                                                1700000000000);
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
// The relay's goldens (web2 `www` SignatureVerifier.test.ts, RelayApiAuthVerifier.test.ts; the heartbeat by its
// layout): an Account ID since multi-relay MR001.
constexpr const char kAccount[] = "account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk";

TEST(RegistrationSignPayloadTest, BuildAccountRevokeSignBytesMatchesGoldenVector) {
  const auto bytes = BuildAccountRevokeSignBytes(kAccount, 1700000000005);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d7265766f6b652d7631000100000000000000336163636f756e743a47666d6f6c44"
            "386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000018bcfe56805");
}

TEST(RegistrationSignPayloadTest, BuildProfileUpdateSignBytesMatchesGoldenVector) {
  const auto bytes = BuildProfileUpdateSignBytes(kAccount, "alice", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d70726f66696c652d7631000100000000000000336163636f756e743a47666d6f6c"
            "44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000000000000005616c6963"
            "650000018bcfe56803");
}

// Layout mirrors the relay server's buildMeshHeartbeatSignBytes.
TEST(RegistrationSignPayloadTest, BuildRelayHeartbeatSignBytesMatchesGoldenVector) {
  const auto bytes = BuildRelayHeartbeatSignBytes(kAccount, "12D3KooWpeer", "active", 1700000000007);
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6865617274626561742d7631000100000000000000336163636f756e743a47666d"
            "6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000000c3132"
            "44334b6f6f577065657200000000000000066163746976650000018bcfe56807");
}

TEST(RegistrationSignPayloadTest, HeartbeatSignBytesBindEveryField) {
  const auto base = BuildRelayHeartbeatSignBytes("account:u1", "p1", "active", 1);
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes("account:u2", "p1", "active", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes("account:u1", "p2", "active", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes("account:u1", "p1", "draining", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes("account:u1", "p1", "active", 2));
}

// Push devices register per relay by Account ID, under the relay API's sign version 2 (MR001): the layout of the
// relay's buildRelayApiDeviceSignBytes, computed independently with a struct script.
TEST(RegistrationSignPayloadTest, BuildRelayApiDeviceSignBytesIsVersion2ByAccount) {
  EXPECT_EQ(BytesToHex(BuildRelayApiDeviceSignBytes(RelayApiOp::DeviceRegister, kAccount, "android", "dev-1", "tok-1",
                                                    1700000000009)),
            "70702d62726f777365723a72656c61792d6170692d76310002030000018bcfe5680900000000000000336163636f756e743a"
            "47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000"
            "07616e64726f696400000000000000056465762d310000000000000005746f6b2d31");
}

} // namespace pbr
