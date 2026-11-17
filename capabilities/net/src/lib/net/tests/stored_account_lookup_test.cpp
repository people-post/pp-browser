// The row a relay keeps for an account, looked up unauthenticated (multi-relay R2 step 2 / R3): a contact's relay is
// held to a public host, as the relays its messages go to (docs/contracts/ACCOUNT_CARD.md § Public-relay policy).
#include "common/tests/ScriptedHttpServer.h"
#include "lib/net/OrgBackendClientsImpl.h"

#include <gtest/gtest.h>

#include <string>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using test::ScriptedHttpServer;
using Reply = ScriptedHttpServer::Reply;

class StoredAccountLookupTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_.SetHandler([](const ScriptedHttpServer::Request&) {
      return Reply{.status = 200,
                   .headers = {},
                   .body = R"({"account_id":"account:bob","nickname":"Bob","account_card":{"issued_at":5}})"};
    });
  }

  std::string Relay() const { return server_.Origin() + "/api/relay"; }

  ScriptedHttpServer server_;
};

TEST_F(StoredAccountLookupTest, OneOfOursIsAskedWhereverItIs) {
  auto row = LookupStoredAccount(Relay(), "account:bob");
  ASSERT_TRUE(row) << row.error().message;
  ASSERT_TRUE(*row);
  EXPECT_EQ((*row)->hit.nickname, "Bob");
  auto card = FetchStoredAccountCard(Relay(), "account:bob");
  ASSERT_TRUE(card && *card);
  EXPECT_EQ((*card)->getIf<int64_t>("issued_at").value_or(0), 5);
  EXPECT_EQ(server_.Requests().size(), 2u);
}

TEST_F(StoredAccountLookupTest, AContactsRelayOffAPublicHostIsNotAsked) {
  EXPECT_FALSE(LookupStoredAccount(Relay(), "account:bob", true)) << Relay();
  EXPECT_FALSE(FetchStoredAccountCard(Relay(), "account:bob", true));
  EXPECT_FALSE(LookupStoredAccount("https://10.0.0.5/api/relay", "account:bob", true));
  EXPECT_TRUE(server_.Requests().empty()) << "refused before any request";
}

} // namespace
} // namespace pbr
