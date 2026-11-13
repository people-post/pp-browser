// The relay half of a locker write (multi-device M023 s3): what the relay verifies.
#include "lib/net/HttpLockerClient.h"

#include "foundation/crypto/CryptoUtil.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

TEST(HttpLockerClientTest, PutSignBytesMatchTheRelaysGoldenVector) {
  // Shared with web2 www __tests__/AccountLocker.test.ts (buildLockerPutSignBytes).
  std::string id;
  for (int i = 0; i < 32; ++i) {
    id += "ab";
  }
  EXPECT_EQ(BytesToHex(BuildLockerPutSignBytes("account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk", id,
                                               1700000000123ULL, 1700000000456LL)),
            "70702d62726f777365723a72656c61792d6c6f636b65722d7075742d7631000100000000000000336163636f756e743a4766"
            "6d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b000000000000004061"
            "6261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261"
            "626162616261626162616261620000018bcfe5687b0000018bcfe569c8");
}

TEST(HttpLockerClientTest, WithoutASignerOrAUrlNothingIsSent) {
  HttpLockerClient unconfigured("");
  EXPECT_FALSE(unconfigured.Put("account:alice", LockerRecord{}));
  EXPECT_FALSE(unconfigured.Get("ab"));
  HttpLockerClient unsigned_client("http://127.0.0.1:9");
  EXPECT_FALSE(unsigned_client.Put("account:alice", LockerRecord{}));
}

} // namespace
} // namespace pbr
