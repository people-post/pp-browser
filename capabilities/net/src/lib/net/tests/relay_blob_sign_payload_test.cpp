#include "foundation/crypto/CryptoUtil.h"
#include "lib/net/RelayBlobSignPayload.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The storage commands' bytes (MR007-AUD: version 2, the relay's PeerId after the version), by its account
// (multi-relay MR001): the hex computed independently with a struct script.
constexpr const char kAudience[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";
constexpr const char kAccount[] = "account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk";

TEST(RelayBlobSignPayloadTest, BuildBlobPresignSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildBlobPresignSignBytes(kAudience, kAccount, "application/octet-stream", 2048, "file",
                                                 1700000000003)),
            "70702d62726f777365723a72656c61792d626c6f622d7072657369676e2d76310002000000000000002e516d54576b535141"
            "63477345546f6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f"
            "756e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000"
            "00000000186170706c69636174696f6e2f6f637465742d73747265616d0000000000000800000000000000000466696c6500"
            "00018bcfe56803");
}

TEST(RelayBlobSignPayloadTest, BuildBlobRetainSignBytesMatchesGoldenVector) {
  EXPECT_EQ(
      BytesToHex(BuildBlobRetainSignBytes(kAudience, kAccount, "00000000-0000-4000-8000-000000000001", 1700000000003)),
      "70702d62726f777365723a72656c61792d626c6f622d72657461696e2d76310002000000000000002e516d54576b53514163"
      "477345546f6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f75"
      "6e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000"
      "0000002430303030303030302d303030302d343030302d383030302d3030303030303030303030310000018bcfe56803");
}

TEST(RelayBlobSignPayloadTest, BuildProfileIconSignBytesMatchesGoldenVector) {
  EXPECT_EQ(BytesToHex(BuildProfileIconSignBytes(kAudience, kAccount, "", "00000000-0000-4000-8000-000000000001",
                                                 "image/jpeg", 1700000000003)),
            "70702d62726f777365723a72656c61792d70726f66696c652d69636f6e2d76310002000000000000002e516d54576b535141"
            "63477345546f6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f"
            "756e743a47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000"
            "0000000000000000000000002430303030303030302d303030302d343030302d383030302d30303030303030303030303100"
            "0000000000000a696d6167652f6a7065670000018bcfe56803");
}

TEST(RelayBlobSignPayloadTest, NoCommandIsBuiltForNoRelay) {
  EXPECT_TRUE(BuildBlobPresignSignBytes("", kAccount, "a/b", 1, "file", 1).empty());
  EXPECT_TRUE(BuildBlobDeleteSignBytes("", kAccount, "b", 1).empty());
  EXPECT_TRUE(BuildBlobListSignBytes("", kAccount, "", 1).empty());
}

} // namespace
} // namespace pbr
