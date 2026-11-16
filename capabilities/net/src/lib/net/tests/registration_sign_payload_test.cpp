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

// The relay the commands are for (multi-relay MR007): the PeerId of 1952 bytes of 0x42 (MESH_IDENTITY's fixture).
constexpr const char kAudience[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";
// The Account ID of the same key (M002's golden).
constexpr const char kAccount[] = "account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk";

} // namespace

// MR007-AUD: each command's bytes name the relay right after the version (expectations computed independently with a
// struct script; the full set, every domain against two relays, is relay_sign_vectors.json).
TEST(RegistrationSignPayloadTest, BuildRegistrationSignBytesNamesTheRelayAndBindsKemKey) {
  const auto bytes = BuildRegistrationSignBytes(kAudience, "ch-1", VectorMlDsaPublicKeyB64(), VectorKemPublicKeyB64(),
                                                "ml-dsa-65", 1700000000000);
  // domain+NUL + ver + len-prefixed audience + len-prefixed "ch-1" + ML-DSA-65 pk + ML-KEM-768 pk + alg + i64 ts
  ASSERT_EQ(bytes.size(), kMlDsa65PublicKeyBytes + kHybridKemPublicKeyBytes + 51u + 8u + 46u);
  EXPECT_EQ(bytes[29], 0x03) << "sign version 3";
  EXPECT_EQ(std::string(bytes.begin() + 38, bytes.begin() + 38 + 46), kAudience);
  EXPECT_EQ(bytes[bytes.size() - 10], 0x43);
  EXPECT_EQ(bytes[bytes.size() - 9], 0x01); // ml-dsa-65 wire alg
  EXPECT_EQ(bytes[bytes.size() - 8], 0x00);
  EXPECT_EQ(bytes[bytes.size() - 1], 0x00);

  const auto other_bytes = BuildRegistrationSignBytes(kAudience, "ch-1", VectorMlDsaPublicKeyB64(),
                                                      VectorKemPublicKeyB64(0x44), "ml-dsa-65", 1700000000000);
  EXPECT_NE(bytes, other_bytes);
}

TEST(RegistrationSignPayloadTest, NoCommandIsBuiltForNoRelay) {
  EXPECT_TRUE(BuildRegistrationSignBytes("", "ch-1", VectorMlDsaPublicKeyB64(), VectorKemPublicKeyB64(), "ml-dsa-65", 1)
                  .empty());
  EXPECT_TRUE(BuildAccountRevokeSignBytes("", kAccount, "reg-1", 1).empty());
  EXPECT_TRUE(BuildProfileUpdateSignBytes("", kAccount, "alice", 1).empty());
  EXPECT_TRUE(BuildRelayHeartbeatSignBytes("", kAccount, "p", "active", 1).empty());
  EXPECT_TRUE(BuildLlmTokenRenewSignBytes("", kAccount, 1).empty());
  EXPECT_TRUE(BuildRelayApiDeviceRegisterSignBytes("", kAccount, "android", "d", "t", 1).empty());
}

TEST(RegistrationSignPayloadTest, TheAudienceIsSigned) {
  EXPECT_NE(BuildProfileUpdateSignBytes(kAudience, kAccount, "alice", 1),
            BuildProfileUpdateSignBytes("QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie", kAccount, "alice", 1));
}

// MR007-SCOPE: a revoke names the registration it removes.
TEST(RegistrationSignPayloadTest, BuildAccountRevokeSignBytesNamesTheRegistration) {
  const auto bytes = BuildAccountRevokeSignBytes(kAudience, kAccount, "reg-1", 1700000000005);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d7265766f6b652d76310002000000000000002e516d54576b53514163477345546f"
            "6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000000572"
            "65672d310000018bcfe56805");
  EXPECT_NE(bytes, BuildAccountRevokeSignBytes(kAudience, kAccount, "reg-2", 1700000000005));
}

TEST(RegistrationSignPayloadTest, BuildProfileUpdateSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildProfileUpdateSignBytes(kAudience, kAccount, "alice", 1700000000003)),
            "70702d62726f777365723a72656c61792d70726f66696c652d76310002000000000000002e516d54576b5351416347734554"
            "6f6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e743a47"
            "666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000000000000005"
            "616c6963650000018bcfe56803");
}

TEST(RegistrationSignPayloadTest, BuildRelayHeartbeatSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildRelayHeartbeatSignBytes(kAudience, kAccount, "12D3KooWpeer", "active", 1700000000007)),
            "70702d62726f777365723a72656c61792d6865617274626561742d76310002000000000000002e516d54576b535141634773"
            "45546f6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e74"
            "3a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000"
            "000c313244334b6f6f577065657200000000000000066163746976650000018bcfe56807");
}

TEST(RegistrationSignPayloadTest, BuildLlmTokenRenewSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildLlmTokenRenewSignBytes(kAudience, kAccount, 1700000000008)),
            "70702d62726f777365723a72656c61792d6c6c6d2d746f6b656e2d76310002000000000000002e516d54576b535141634773"
            "45546f6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e74"
            "3a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000018bcfe5"
            "6808");
}

TEST(RegistrationSignPayloadTest, HeartbeatSignBytesBindEveryField) {
  const auto base = BuildRelayHeartbeatSignBytes(kAudience, "account:u1", "p1", "active", 1);
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes(kAudience, "account:u2", "p1", "active", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes(kAudience, "account:u1", "p2", "active", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes(kAudience, "account:u1", "p1", "draining", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes(kAudience, "account:u1", "p1", "active", 2));
}

// Push devices register per relay by Account ID, under the relay API's sign version 3 (MR007); an unregister names
// the binding it removes (MR007-SCOPE).
TEST(RegistrationSignPayloadTest, BuildRelayApiDeviceSignBytesAreVersion3) {
  EXPECT_EQ(
      BytesToHex(BuildRelayApiDeviceRegisterSignBytes(kAudience, kAccount, "android", "dev-1", "tok-1", 1700000000009)),
      "70702d62726f777365723a72656c61792d6170692d76310003000000000000002e516d54576b53514163477345546f675446"
      "4b72504a33477a644847337650395577585679703248654b3959427650030000018bcfe5680900000000000000336163636f"
      "756e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000"
      "0000000007616e64726f696400000000000000056465762d310000000000000005746f6b2d31");
  EXPECT_EQ(BytesToHex(BuildRelayApiDeviceUnregisterSignBytes(kAudience, kAccount, "android", "dev-1", "bind-1",
                                                              1700000000009)),
            "70702d62726f777365723a72656c61792d6170692d76310003000000000000002e516d54576b53514163477345546f675446"
            "4b72504a33477a644847337650395577585679703248654b3959427650040000018bcfe5680900000000000000336163636f"
            "756e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000"
            "0000000007616e64726f696400000000000000056465762d31000000000000000662696e642d31");
}

} // namespace pbr
