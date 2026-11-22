// The relay half of a locker write (multi-device M023 s3): what the relay verifies.
#include "lib/net/HttpLockerClient.h"

#include "common/tests/ScriptedHttpServer.h"
#include "foundation/crypto/CryptoUtil.h"

#include "lib/net/testing/FakeRelaySigner.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char kAudience[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";

// MR007-AUD: the quota half names the relay (version 4, the empty area last, SIGNED_FORMATS § 2.8; computed
// independently with a struct script).
TEST(HttpLockerClientTest, PutSignBytesMatchTheGoldenVector) {
  std::string id;
  for (int i = 0; i < 32; ++i) {
    id += "ab";
  }
  EXPECT_EQ(BytesToHex(BuildLockerPutSignBytes(kAudience, "account:GfmolD8fsSJ4FcK1d_6wOhTbZaVr0_ELv9iiZG0_Idk", id,
                                               1700000000123ULL, 1700000000456LL)),
            "70702d62726f777365723a72656c61792d6c6f636b65722d7075740004000000000000002e516d54576b5351416347734554"
            "6f6754464b72504a33477a644847337650395577585679703248654b395942765000000000000000336163636f756e743a47"
            "666d6f6c44386673534a3446634b31645f36774f6854625a615672305f454c763969695a47305f49646b0000000000000040"
            "6162616261626162616261626162616261626162616261626162616261626162616261626162616261626162616261626162"
            "61626162616261626162616261620000018bcfe5687b0000018bcfe569c80000000000000000");
  EXPECT_TRUE(BuildLockerPutSignBytes("", "account:a", id, 1, 1).empty()) << "no relay, no bytes";
}

TEST(HttpLockerClientTest, WithoutASignerAnAudienceOrAUrlNothingIsSent) {
  HttpLockerClient unconfigured("");
  EXPECT_FALSE(unconfigured.Put("account:alice", LockerRecord{}));
  EXPECT_FALSE(unconfigured.Get("ab"));
  HttpLockerClient unsigned_client("http://127.0.0.1:9");
  unsigned_client.SetAudience(std::make_shared<FixedRelayAudience>(kAudience));
  EXPECT_FALSE(unsigned_client.Put("account:alice", LockerRecord{}));
  HttpLockerClient no_audience("http://127.0.0.1:9");
  no_audience.SetAuthSigner(test::FakeRelaySigner());
  EXPECT_FALSE(no_audience.Put("account:alice", LockerRecord{}));
}

// X12: a 409 names the version the relay holds (SPECS § lockers/put), so the next write can go above it.
TEST(HttpLockerClientTest, ANotNewerAnswerNamesTheStoredVersion) {
  test::ScriptedHttpServer server;
  std::string body = R"({"error":"A newer or equal version is stored","version":1700000000999})";
  server.SetHandler([&body](const test::ScriptedHttpServer::Request&) {
    return test::ScriptedHttpServer::Reply{.status = 409, .headers = {}, .body = body};
  });
  HttpLockerClient client(server.Origin());
  client.SetAudience(std::make_shared<FixedRelayAudience>(kAudience));
  client.SetAuthSigner(test::FakeRelaySigner());
  LockerRecord record;
  record.locker_id = std::string(64, 'a');
  record.version = 5;
  std::optional<LockerPutRefused> refused;
  EXPECT_FALSE(client.Put("account:alice", record, &refused));
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->reason, LockerPutRefusal::NotNewer);
  EXPECT_EQ(refused->stored_version, 1700000000999u);

  body = R"({"error":"A newer or equal version is stored"})";
  refused.reset();
  EXPECT_FALSE(client.Put("account:alice", record, &refused));
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->stored_version, 0u) << "a relay that does not say";
}

} // namespace
} // namespace pbr
