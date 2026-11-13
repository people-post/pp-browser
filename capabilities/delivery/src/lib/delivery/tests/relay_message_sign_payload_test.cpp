#include "lib/delivery/RelayGrantSigner.h"
#include "lib/delivery/RelayMessageSignPayload.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Golden vectors shared with the relay (web2 `www` app/__tests__/SignatureVerifier.test.ts and
// RelayApiAuthVerifier.test.ts, multi-relay MR001 / MR005): sign version 2, every party an Account ID.

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
  const auto bytes = BuildRelayApiSendSignBytes(MakeSendVectorRecord(), 1700000000001);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6170692d76310002000000018bcfe5680100000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000336163636f"
            "756e743a7258526c385f4375537a704d3146493135534d34687a726379354e6a4a4f703266646d6541366d646d4c30000000000000"
            "000873747265616d2d310000000000000007");
}

TEST(RelayApiSignPayloadTest, TheSendSignsNeitherTheGrantNorTheSendersKey) {
  RelayWireSendRecord with_extras = MakeSendVectorRecord();
  with_extras.grant = RelayGrant{.recipient_account_id = kGoldenRecipient,
                                 .sender_account_id = kGoldenAccount,
                                 .issued_at = 1,
                                 .expires_at = 2,
                                 .signature = "sig"};
  with_extras.sender_public_key_b64 = "key";
  EXPECT_EQ(BuildRelayApiSendSignBytes(with_extras, 1700000000001),
            BuildRelayApiSendSignBytes(MakeSendVectorRecord(), 1700000000001));
}

TEST(RelayApiSignPayloadTest, BuildPollInboxSignBytesMatchesGoldenVector) {
  const auto bytes = BuildRelayApiPollInboxSignBytes(kGoldenAccount, "", 1700000000002);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6170692d76310002010000018bcfe5680200000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000000000000000");
}

TEST(RelayApiSignPayloadTest, BuildAckInboxSignBytesMatchesGoldenVector) {
  const auto bytes = BuildRelayApiAckInboxSignBytes(kGoldenAccount, "cur-1", 1700000000004);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6170692d76310002050000018bcfe5680400000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000056375722d"
            "31");
}

TEST(RelayApiSignPayloadTest, BuildClearInboxSignBytesMatchesGoldenVector) {
  const auto bytes = BuildRelayApiClearInboxSignBytes(kGoldenAccount, "2024-01-01T00:00:00.000Z", 1700000000005);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6170692d76310002060000018bcfe5680500000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000001832303234"
            "2d30312d30315430303a30303a30302e3030305a");
}

// MR005: the grant's sign bytes, as the relay checks them (RelayApiAuthVerifier.test.ts "grant sign bytes match the
// golden vector"): the recipient grants the sender, 30 days from 2025-10-23T16:00:00Z.
TEST(RelayGrantSignBytesTest, MatchTheRelaysGoldenVector) {
  constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;
  const RelayGrant grant{.recipient_account_id = kGoldenAccount,
                         .sender_account_id = kGoldenRecipient,
                         .issued_at = 1761235200000,
                         .expires_at = 1761235200000 + 30 * kDayMs};
  EXPECT_EQ(BytesToHex(BuildRelayGrantSignBytes(grant)),
            "70702d62726f777365723a72656c61792d6772616e742d7631000100000000000000336163636f756e743a47666d6f6c443866"
            "73534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000336163636f756e743a72"
            "58526c385f4375537a704d3146493135534d34687a726379354e6a4a4f703266646d6541366d646d4c300000019a11cc68000000"
            "019aac4b3000");
}

} // namespace pbr
