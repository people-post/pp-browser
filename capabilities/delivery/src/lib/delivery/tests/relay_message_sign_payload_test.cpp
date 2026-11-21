#include "lib/delivery/RelayGrantSigner.h"
#include "lib/delivery/RelayMessageSignPayload.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The message API's bytes (MR007-AUD: sign version 3, the relay's PeerId after the version; every party an Account ID,
// MR001), computed independently with a struct script. The relay checks the full set in relay_sign_vectors.json.

constexpr const char kAudience[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";

/** The Account ID of 1952 bytes of 0x42 (M002's cross-repo golden). */
constexpr const char kGoldenAccount[] = "account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk";
/** The Account ID of 1952 bytes of 0x01: the golden sends' recipient. */
constexpr const char kGoldenRecipient[] = "account:rXRl8_CuSzpM1FI15SM4hzrcy5NjJOp2fdmeA6mdmL0";

RelayWireSendRecord MakeSendVectorRecord() {
  RelayWireSendRecord record;
  record.sender_account_id = kGoldenAccount;
  record.recipient_account_id = kGoldenRecipient;
  record.stream_id = "stream-1";
  record.index_key = 7;
  return record;
}

} // namespace

TEST(RelayApiSignPayloadTest, TheGoldenAccountsAreTheirKeysAccountIds) {
  EXPECT_EQ(AccountIdFromMlDsaPublicKey(ByteVector(kMlDsa65PublicKeyBytes, 0x42)).valueOr(""), kGoldenAccount);
  EXPECT_EQ(AccountIdFromMlDsaPublicKey(ByteVector(kMlDsa65PublicKeyBytes, 0x01)).valueOr(""), kGoldenRecipient);
}

TEST(RelayApiSignPayloadTest, BuildSendSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildRelayApiSendSignBytes(kAudience, MakeSendVectorRecord(), 1700000000001)),
            "70702d62726f777365723a72656c61792d6170692d76310003000000000000002e516d54576b53514163477345546f675446"
            "4b72504a33477a644847337650395577585679703248654b3959427650000000018bcfe5680100000000000000336163636f"
            "756e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000"
            "00000000336163636f756e743a7258526c385f4375537a704d3146493135534d34687a726379354e6a4a4f703266646d6541"
            "366d646d4c30000000000000000873747265616d2d310000000000000007");
}

TEST(RelayApiSignPayloadTest, TheSendSignsNeitherTheGrantNorTheSendersKey) {
  RelayWireSendRecord with_extras = MakeSendVectorRecord();
  with_extras.grant = RelayGrant{.recipient_account_id = kGoldenRecipient,
                                 .sender_account_id = kGoldenAccount,
                                 .issued_at = 1,
                                 .expires_at = 2,
                                 .signature = "sig"};
  with_extras.sender_public_key_b64 = "key";
  with_extras.audience = "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie"; // the body's copy: the bytes use their own
  EXPECT_EQ(BuildRelayApiSendSignBytes(kAudience, with_extras, 1700000000001),
            BuildRelayApiSendSignBytes(kAudience, MakeSendVectorRecord(), 1700000000001));
  EXPECT_TRUE(BuildRelayApiSendSignBytes("", MakeSendVectorRecord(), 1700000000001).empty()) << "no relay, no bytes";
}

TEST(RelayApiSignPayloadTest, BuildPollInboxSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildRelayApiPollInboxSignBytes(kAudience, kGoldenAccount, "", 1700000000002)),
            "70702d62726f777365723a72656c61792d6170692d76310003000000000000002e516d54576b53514163477345546f675446"
            "4b72504a33477a644847337650395577585679703248654b3959427650010000018bcfe5680200000000000000336163636f"
            "756e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000"
            "0000000000");
}

TEST(RelayApiSignPayloadTest, BuildAckInboxSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildRelayApiAckInboxSignBytes(kAudience, kGoldenAccount, "cur-1", 1700000000004)),
            "70702d62726f777365723a72656c61792d6170692d76310003000000000000002e516d54576b53514163477345546f675446"
            "4b72504a33477a644847337650395577585679703248654b3959427650050000018bcfe5680400000000000000336163636f"
            "756e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000"
            "00000000056375722d31");
}

TEST(RelayApiSignPayloadTest, BuildClearInboxSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(
                BuildRelayApiClearInboxSignBytes(kAudience, kGoldenAccount, "2024-01-01T00:00:00.000Z", 1700000000005)),
            "70702d62726f777365723a72656c61792d6170692d76310003000000000000002e516d54576b53514163477345546f675446"
            "4b72504a33477a644847337650395577585679703248654b3959427650060000018bcfe5680500000000000000336163636f"
            "756e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000"
            "0000000018323032342d30312d30315430303a30303a30302e3030305a");
}

} // namespace pbr
