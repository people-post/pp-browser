#include "foundation/crypto/CryptoUtil.h"
#include "base/net/RelayBlobSignPayload.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(RelayBlobSignPayloadTest, BuildBlobPresignSignBytesMatchesGoldenVector) {
  const auto bytes = BuildBlobPresignSignBytes("relay:u1", "application/octet-stream", 2048, "file", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d626c6f622d7072657369676e2d76310001000000000000000872656c61793a"
            "753100000000000000186170706c69636174696f6e2f6f637465742d73747265616d000000000000080000000000000000"
            "0466696c650000018bcfe56803");
}

TEST(RelayBlobSignPayloadTest, BuildBlobRetainSignBytesMatchesGoldenVector) {
  const auto bytes =
      BuildBlobRetainSignBytes("relay:u1", "00000000-0000-4000-8000-000000000001", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d626c6f622d72657461696e2d76310001000000000000000872656c61793a"
            "7531000000000000002430303030303030302d303030302d343030302d383030302d303030303030303030303031000001"
            "8bcfe56803");
}

TEST(RelayBlobSignPayloadTest, BuildProfileIconSignBytesMatchesGoldenVector) {
  const auto bytes = BuildProfileIconSignBytes("relay:u1", "", "00000000-0000-4000-8000-000000000001",
                                               "image/jpeg", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d70726f66696c652d69636f6e2d76310001000000000000000872656c61793a"
            "75310000000000000000000000000000002430303030303030302d303030302d343030302d383030302d303030303030"
            "303030303031000000000000000a696d6167652f6a7065670000018bcfe56803");
}

} // namespace
} // namespace pbr
