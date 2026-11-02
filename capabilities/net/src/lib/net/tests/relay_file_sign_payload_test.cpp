#include "foundation/crypto/CryptoUtil.h"
#include "lib/net/RelayBlobSignPayload.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Golden vectors of the relay (app-static-api app/__tests__/RelayFiles.test.ts, "relay file sign bytes"): the
// server verifies these exact bytes, so one byte off is a 401 on every attachment.
constexpr const char kUser[] = "relay:golden1";
constexpr const char kBlob[] = "11111111-2222-3333-4444-555555555555";
constexpr int64_t kTimestamp = 1761900000000;

TEST(RelayFileSignPayloadTest, StartMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFileStartSignBytes(kUser, 1048576, kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d73746172742d76310001000000000000000d72656c61793a676f6c64"
            "656e3100000000001000000000019a396c7300");
}

TEST(RelayFileSignPayloadTest, CompleteMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFileCompleteSignBytes(kUser, kBlob, kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d636f6d706c6574652d76310001000000000000000d72656c61793a67"
            "6f6c64656e31000000000000002431313131313131312d323232322d333333332d343434342d353535353535353535353535"
            "0000019a396c7300");
}

TEST(RelayFileSignPayloadTest, DeleteMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFileDeleteSignBytes(kUser, kBlob, kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d64656c6574652d76310001000000000000000d72656c61793a676f6c"
            "64656e31000000000000002431313131313131312d323232322d333333332d343434342d3535353535353535353535350000"
            "019a396c7300");
}

TEST(RelayFileSignPayloadTest, ListMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFileListSignBytes(kUser, "pending", kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d6c6973742d76310001000000000000000d72656c61793a676f6c6465"
            "6e31000000000000000770656e64696e670000019a396c7300");
}

TEST(RelayFileSignPayloadTest, AnUnfilteredListSignsAnEmptyStatus) {
  const auto bytes = BytesToHex(BuildFileListSignBytes(kUser, "", kTimestamp));
  EXPECT_NE(bytes.find("0000000000000000"
                       "0000019a396c7300"),
            std::string::npos)
      << bytes;
}

} // namespace
} // namespace pbr
