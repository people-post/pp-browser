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
  EXPECT_EQ(BytesToHex(BuildLockerPutSignBytes("relay:alice", id, 1700000000123ULL, 1700000000456LL)),
            "70702d62726f777365723a72656c61792d6c6f636b65722d7075742d76310001000000000000000b72656c61793a616c6963650000000000000040616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261620000018bcfe5687b0000018bcfe569c8");
}

TEST(HttpLockerClientTest, WithoutASignerOrAUrlNothingIsSent) {
  HttpLockerClient unconfigured("");
  EXPECT_FALSE(unconfigured.Put("relay:alice", LockerRecord{}));
  EXPECT_FALSE(unconfigured.Get("ab"));
  HttpLockerClient unsigned_client("http://127.0.0.1:9");
  EXPECT_FALSE(unsigned_client.Put("relay:alice", LockerRecord{}));
}

} // namespace
} // namespace pbr
