#include "foundation/crypto/CryptoUtil.h"
#include "lib/net/RelayBlobSignPayload.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The relay's sign layout (web2 `www` SignatureVerifier.ts), by its account (multi-relay MR001): the hex computed
// independently with a struct script, as the relay's own goldens are.
constexpr const char kAccount[] = "account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk";

TEST(RelayBlobSignPayloadTest, BuildBlobPresignSignBytesMatchesGoldenVector) {
  const auto bytes = BuildBlobPresignSignBytes(kAccount, "application/octet-stream", 2048, "file", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d626c6f622d7072657369676e2d7631000100000000000000336163636f756e743a"
            "47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000"
            "186170706c69636174696f6e2f6f637465742d73747265616d0000000000000800000000000000000466696c650000018bcf"
            "e56803");
}

TEST(RelayBlobSignPayloadTest, BuildBlobRetainSignBytesMatchesGoldenVector) {
  const auto bytes = BuildBlobRetainSignBytes(kAccount, "00000000-0000-4000-8000-000000000001", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d626c6f622d72657461696e2d7631000100000000000000336163636f756e743a47"
            "666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000000000000024"
            "30303030303030302d303030302d343030302d383030302d3030303030303030303030310000018bcfe56803");
}

TEST(RelayBlobSignPayloadTest, BuildProfileIconSignBytesMatchesGoldenVector) {
  const auto bytes =
      BuildProfileIconSignBytes(kAccount, "", "00000000-0000-4000-8000-000000000001", "image/jpeg", 1700000000003);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(BytesToHex(bytes),
            "70702d62726f777365723a72656c61792d70726f66696c652d69636f6e2d7631000100000000000000336163636f756e743a"
            "47666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b00000000000000"
            "00000000000000002430303030303030302d303030302d343030302d383030302d3030303030303030303030310000000000"
            "00000a696d6167652f6a7065670000018bcfe56803");
}

} // namespace
} // namespace pbr
