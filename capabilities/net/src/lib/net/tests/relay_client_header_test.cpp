#include "common/tests/ScriptedHttpServer.h"
#include "foundation/tls/RelayClientHeader.h"
#include "lib/net/HttpClient.h"
#include "lib/net/HttpLockerClient.h"
#include "lib/net/RelayIdentity.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// O1 (SERVICE_ENDPOINTS § Version header): every relay call names the build in `X-PP-Client`; a call to a host that is
// not a relay (a peer's link: HttpClient used directly) does not, and nothing is sent while no value is set.

constexpr char kHeader[] = "pp-app/1.0.0-rc16 linux";

class RelayClientHeaderTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_.SetHandler([](const test::ScriptedHttpServer::Request&) {
      return test::ScriptedHttpServer::Reply{.status = 404, .headers = {}, .body = "{}"};
    });
  }
  std::string LastHead() const {
    const auto requests = server_.Requests();
    return requests.empty() ? std::string() : requests.back().head;
  }
  test::ScriptedHttpServer server_;
};

TEST(RelayClientHeaderPolicyTest, AddedOnlyWhenSetAndNeverOverACallersOwn) {
  {
    const ScopedRelayClientHeaderForTest unset("");
    EXPECT_TRUE(WithRelayClientHeader().empty());
  }
  const ScopedRelayClientHeaderForTest set(kHeader);
  const auto added = WithRelayClientHeader({{"Content-Type", "application/json"}});
  EXPECT_EQ(added.at("X-PP-Client"), kHeader);
  EXPECT_EQ(added.at("Content-Type"), "application/json");
  const auto kept = WithRelayClientHeader({{"x-pp-client", "caller"}});
  ASSERT_EQ(kept.size(), 1u);
  EXPECT_EQ(kept.at("x-pp-client"), "caller");
}

TEST_F(RelayClientHeaderTest, RelayCallsCarryItAndOtherCallsDoNot) {
  const ScopedRelayClientHeaderForTest set(kHeader);
  HttpLockerClient locker(server_.Origin());
  (void)locker.Get(std::string(64, 'a'));
  EXPECT_NE(LastHead().find(std::string("X-PP-Client: ") + kHeader), std::string::npos) << LastHead();
  (void)FetchRelayIdentity(server_.Origin());
  EXPECT_NE(LastHead().find(std::string("X-PP-Client: ") + kHeader), std::string::npos) << LastHead();
  ASSERT_TRUE(HttpClient::Get(server_.Origin() + "/a-peers-link"));
  EXPECT_EQ(LastHead().find("X-PP-Client"), std::string::npos) << "not a relay call";
}

TEST_F(RelayClientHeaderTest, NothingIsSentWhileNoValueIsSet) {
  const ScopedRelayClientHeaderForTest unset("");
  HttpLockerClient locker(server_.Origin());
  (void)locker.Get(std::string(64, 'a'));
  EXPECT_EQ(LastHead().find("X-PP-Client"), std::string::npos);
}

} // namespace
} // namespace pbr
