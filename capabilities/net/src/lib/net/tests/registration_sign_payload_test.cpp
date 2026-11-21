#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/TypedBlob.h"
#include "lib/net/RegistrationSignPayload.h"
#include "lib/net/RelayApiSignPayload.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

/** A typed ML-DSA-65 key of one fill byte (SIGNED_FORMATS § 1.3: the algorithm id, then the key). */
std::string VectorMlDsaPublicKeyB64(uint8_t fill = 0x42) {
  return Base64Encode(TypedSigningPublicKey(std::vector<uint8_t>(kMlDsa65PublicKeyBytes, fill)).value());
}

/** A typed ML-KEM-768 key of one fill byte. */
std::string VectorKemPublicKeyB64(uint8_t fill = 0x43) {
  return Base64Encode(TypedKemPublicKey(std::vector<uint8_t>(kHybridKemPublicKeyBytes, fill)).value());
}

// The relay the commands are for (multi-relay MR007): the PeerId of 1952 bytes of 0x42 (MESH_IDENTITY's fixture).
constexpr const char kAudience[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";
// The Account ID of the same key (M002's golden).
constexpr const char kAccount[] = "account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk";

} // namespace

// MR007-AUD, SIGNED_FORMATS § 2.8: register's bytes name the relay right after the version (4), then the challenge
// and the account's typed keys as LenBytes, the timestamp and the extension area; no `signature_alg` byte (the full set,
// every domain against two relays, is relay_sign_vectors.json).
TEST(RegistrationSignPayloadTest, BuildRegistrationSignBytesNamesTheRelayAndBindsKemKey) {
  const auto bytes =
      BuildRegistrationSignBytes(kAudience, "ch-1", VectorMlDsaPublicKeyB64(), VectorKemPublicKeyB64(), 1700000000001);
  // "pp-browser:relay-register" 00 04 (27) ‖ LenUtf8 audience (54) ‖ LenUtf8 "ch-1" (12) ‖ LenBytes typed key
  // (8 + 1953) ‖ LenBytes typed KEM key (8 + 1185) ‖ i64 timestamp ‖ the empty area (u64 0)
  ASSERT_EQ(bytes.size(), 27u + 54u + 12u + 1961u + 1193u + 8u + 8u);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 25), "pp-browser:relay-register");
  EXPECT_EQ(bytes[25], 0x00);
  EXPECT_EQ(bytes[26], 0x04) << "sign version 4";
  EXPECT_EQ(std::string(bytes.begin() + 35, bytes.begin() + 35 + 46), kAudience);
  EXPECT_EQ(std::string(bytes.begin() + 89, bytes.begin() + 93), "ch-1");
  EXPECT_EQ(bytes[99], 0x07) << "the typed key's length, 1953";
  EXPECT_EQ(bytes[100], 0xa1);
  EXPECT_EQ(bytes[101], 0x01) << "ml-dsa-65's id first";
  EXPECT_EQ(bytes[102], 0x42);
  EXPECT_EQ(bytes[2060], 0x04) << "the typed KEM key's length, 1185";
  EXPECT_EQ(bytes[2061], 0xa1);
  EXPECT_EQ(bytes[2062], 0x01) << "ml-kem-768's id first";
  EXPECT_EQ(bytes[2063], 0x43);
  EXPECT_EQ(bytes[bytes.size() - 9], 0x01) << "the timestamp's last byte";
  EXPECT_EQ(std::vector<uint8_t>(bytes.end() - 8, bytes.end()), std::vector<uint8_t>(8, 0)) << "the empty area";

  EXPECT_NE(bytes, BuildRegistrationSignBytes(kAudience, "ch-1", VectorMlDsaPublicKeyB64(), VectorKemPublicKeyB64(0x44),
                                              1700000000001));
}

// SIGNED_FORMATS § 1.3: the keys are typed blobs; a bare key, or one of an algorithm this build does not write, builds
// nothing.
TEST(RegistrationSignPayloadTest, RegistrationTakesTypedKeysOnly) {
  const std::string bare = Base64Encode(std::vector<uint8_t>(kMlDsa65PublicKeyBytes, 0x42));
  EXPECT_TRUE(BuildRegistrationSignBytes(kAudience, "ch-1", bare, VectorKemPublicKeyB64(), 1).empty());
  auto unknown = Base64Decode(VectorMlDsaPublicKeyB64()).value();
  unknown[0] = 0x02;
  EXPECT_TRUE(BuildRegistrationSignBytes(kAudience, "ch-1", Base64Encode(unknown), VectorKemPublicKeyB64(), 1).empty());
  EXPECT_TRUE(BuildRegistrationSignBytes(kAudience, "ch-1", VectorMlDsaPublicKeyB64(),
                                         Base64Encode(std::vector<uint8_t>(kHybridKemPublicKeyBytes, 0x43)), 1)
                  .empty());
}

// The extension area is the last field, signed (SIGNED_FORMATS § 1.2).
TEST(RegistrationSignPayloadTest, TheExtensionAreaIsTheLastFieldSigned) {
  const pp::ExtArea ext = pp::ExtArea::FromEntries({{.type = 0x0042, .value = {0xca, 0xfe}}}).value();
  const auto plain = BuildProfileUpdateSignBytes(kAudience, kAccount, "alice", 1700000000003);
  const auto with_entry = BuildProfileUpdateSignBytes(kAudience, kAccount, "alice", 1700000000003, ext);
  ASSERT_EQ(with_entry.size(), plain.size() + 8u);
  EXPECT_EQ(std::vector<uint8_t>(with_entry.begin(), with_entry.end() - 16),
            std::vector<uint8_t>(plain.begin(), plain.end() - 8));
  EXPECT_EQ(BytesToHex(std::vector<uint8_t>(with_entry.end() - 16, with_entry.end())),
            "0000000000000008004200000002cafe");
}

TEST(RegistrationSignPayloadTest, NoCommandIsBuiltForNoRelay) {
  EXPECT_TRUE(BuildRegistrationSignBytes("", "ch-1", VectorMlDsaPublicKeyB64(), VectorKemPublicKeyB64(), 1).empty());
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
            "70702d62726f777365723a72656c61792d7265766f6b650004000000000000002e516d54576b53514163477345546f675446"
            "4b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e743a47666d6f6c"
            "44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000057265672d"
            "310000018bcfe568050000000000000000");
  EXPECT_NE(bytes, BuildAccountRevokeSignBytes(kAudience, kAccount, "reg-2", 1700000000005));
}

TEST(RegistrationSignPayloadTest, BuildProfileUpdateSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildProfileUpdateSignBytes(kAudience, kAccount, "alice", 1700000000003)),
            "70702d62726f777365723a72656c61792d70726f66696c650004000000000000002e516d54576b53514163477345546f6754"
            "464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e743a47666d6f"
            "6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000000000000005616c69"
            "63650000018bcfe568030000000000000000");
}

TEST(RegistrationSignPayloadTest, BuildRelayHeartbeatSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildRelayHeartbeatSignBytes(kAudience, kAccount, "12D3KooWpeer", "active", 1700000000007)),
            "70702d62726f777365723a72656c61792d6865617274626561740004000000000000002e516d54576b53514163477345546f"
            "6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000000c31"
            "3244334b6f6f577065657200000000000000066163746976650000018bcfe568070000000000000000");
}

TEST(RegistrationSignPayloadTest, BuildLlmTokenRenewSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildLlmTokenRenewSignBytes(kAudience, kAccount, 1700000000008)),
            "70702d62726f777365723a72656c61792d6c6c6d2d746f6b656e0004000000000000002e516d54576b53514163477345546f"
            "6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000018bcfe5680800"
            "00000000000000");
}

TEST(RegistrationSignPayloadTest, HeartbeatSignBytesBindEveryField) {
  const auto base = BuildRelayHeartbeatSignBytes(kAudience, "account:u1", "p1", "active", 1);
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes(kAudience, "account:u2", "p1", "active", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes(kAudience, "account:u1", "p2", "active", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes(kAudience, "account:u1", "p1", "draining", 1));
  EXPECT_NE(base, BuildRelayHeartbeatSignBytes(kAudience, "account:u1", "p1", "active", 2));
}

// Push devices register per relay by Account ID, under the relay API's sign version 4 (MR007, SIGNED_FORMATS § 2.8);
// an unregister names the binding it removes (MR007-SCOPE).
TEST(RegistrationSignPayloadTest, BuildRelayApiDeviceSignBytesAreVersion4) {
  EXPECT_EQ(
      BytesToHex(BuildRelayApiDeviceRegisterSignBytes(kAudience, kAccount, "android", "dev-1", "tok-1", 1700000000009)),
      "70702d62726f777365723a72656c61792d6170690004000000000000002e516d54576b53514163477345546f6754464b7250"
      "4a33477a644847337650395577585679703248654b3959427650030000018bcfe5680900000000000000336163636f756e74"
      "3a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000"
      "0007616e64726f696400000000000000056465762d310000000000000005746f6b2d310000000000000000");
  EXPECT_EQ(BytesToHex(BuildRelayApiDeviceUnregisterSignBytes(kAudience, kAccount, "android", "dev-1", "bind-1",
                                                              1700000000009)),
            "70702d62726f777365723a72656c61792d6170690004000000000000002e516d54576b53514163477345546f6754464b7250"
            "4a33477a644847337650395577585679703248654b3959427650040000018bcfe5680900000000000000336163636f756e74"
            "3a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000"
            "0007616e64726f696400000000000000056465762d31000000000000000662696e642d310000000000000000");
}

} // namespace pbr
