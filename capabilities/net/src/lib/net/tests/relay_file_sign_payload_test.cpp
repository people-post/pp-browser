#include "foundation/crypto/CryptoUtil.h"
#include "lib/net/RelayBlobSignPayload.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Golden vectors of the relay (web2 `www` app/__tests__/RelayFiles.test.ts, "relay file sign bytes"; an Account ID
// since multi-relay MR001): the server verifies these exact bytes, so one byte off is a 401 on every attachment.
constexpr const char kUser[] = "account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk";
constexpr const char kBlob[] = "11111111-2222-3333-4444-555555555555";
constexpr int64_t kTimestamp = 1761900000000;

TEST(RelayFileSignPayloadTest, StartMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFileStartSignBytes(kUser, 1048576, kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d73746172742d7631000100000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000010000000"
            "00019a396c7300");
}

TEST(RelayFileSignPayloadTest, PartMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFilePartSignBytes(kUser, kBlob, 3, kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d706172742d7631000100000000000000336163636f756e743a47666d"
            "6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000243131"
            "3131313131312d323232322d333333332d343434342d35353535353535353535353500000000000000030000019a396c7300");
}

TEST(RelayFileSignPayloadTest, CompleteMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFileCompleteSignBytes(kUser, kBlob, kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d636f6d706c6574652d7631000100000000000000336163636f756e74"
            "3a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000"
            "002431313131313131312d323232322d333333332d343434342d3535353535353535353535350000019a396c7300");
}

TEST(RelayFileSignPayloadTest, DeleteMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFileDeleteSignBytes(kUser, kBlob, kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d64656c6574652d7631000100000000000000336163636f756e743a47"
            "666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000000000000024"
            "31313131313131312d323232322d333333332d343434342d3535353535353535353535350000019a396c7300");
}

TEST(RelayFileSignPayloadTest, ListMatchesTheServersGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildFileListSignBytes(kUser, "pending", kTimestamp)),
            "70702d62726f777365723a72656c61792d66696c652d6c6973742d7631000100000000000000336163636f756e743a47666d"
            "6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000077065"
            "6e64696e670000019a396c7300");
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
