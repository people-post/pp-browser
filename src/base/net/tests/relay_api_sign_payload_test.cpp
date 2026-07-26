#include "base/crypto/CryptoUtil.h"
#include "base/net/RelayApiSignPayload.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

RelayWireSendRecord MakeSendVectorRecord() {
  RelayWireSendRecord record;
  record.sender_contact_id = "relay:a";
  record.recipient_contact_id = "relay:b";
  record.stream_id = "stream-1";
  record.index_key = 7;
  return record;
}

} // namespace

TEST(RelayApiSignPayloadTest, BuildSendSignBytesMatchesGoldenVector) {
  const auto bytes = BuildRelayApiSendSignBytes(MakeSendVectorRecord(), 1700000000001);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6170692d76310001000000018bcfe568010000000000000007"
            "72656c61793a61000000000000000772656c61793a62000000000000000873747265616d2d310000000000000007");
}

TEST(RelayApiSignPayloadTest, BuildPollInboxSignBytesMatchesGoldenVector) {
  const auto bytes = BuildRelayApiPollInboxSignBytes("relay:a", "", 1700000000002);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6170692d76310001010000018bcfe568020000000000000007"
            "72656c61793a610000000000000000");
}

TEST(RelayApiSignPayloadTest, BuildAckInboxSignBytesMatchesGoldenVector) {
  const auto bytes = BuildRelayApiAckInboxSignBytes("relay:a", "cur-1", 1700000000004);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6170692d76310001050000018bcfe568040000000000000007"
            "72656c61793a6100000000000000056375722d31");
}

TEST(RelayApiSignPayloadTest, BuildClearInboxSignBytesMatchesGoldenVector) {
  const auto bytes =
      BuildRelayApiClearInboxSignBytes("relay:a", "2024-01-01T00:00:00.000Z", 1700000000005);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d6170692d76310001060000018bcfe568050000000000000007"
            "72656c61793a610000000000000018323032342d30312d30315430303a30303a30302e3030305a");
}

} // namespace pbr
