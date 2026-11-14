// The relay grant store port (multi-relay MR005, CR024): one contract on the SQLite store and the in-memory one, and
// the SQLite one's rows surviving a restart.
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/storage/SqliteRelayGrantStore.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace pbr {
namespace {

RelayGrant Grant(const std::string& recipient, const std::string& sender, const int64_t issued_at) {
  return RelayGrant{.recipient_account_id = recipient,
                    .sender_account_id = sender,
                    .issued_at = issued_at,
                    .expires_at = issued_at + kRelayGrantMaxLifetimeMs,
                    .signature = "c2ln-" + std::to_string(issued_at)};
}

enum class Impl { Sqlite, Memory };

class RelayGrantStoreContractTest : public ::testing::TestWithParam<Impl> {
protected:
  void SetUp() override {
    if (GetParam() == Impl::Sqlite) {
      dir_ = std::filesystem::temp_directory_path() / ("pp_relay_grant_contract_" + util::GenerateUuid());
      std::filesystem::create_directories(dir_);
      store_ = std::make_unique<SqliteRelayGrantStore>(Db());
    } else {
      store_ = std::make_unique<InMemoryRelayGrantStore>();
    }
  }
  void TearDown() override {
    store_.reset();
    if (!dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
    }
  }
  std::string Db() const { return (dir_ / "profile.db").string(); }

  std::filesystem::path dir_;
  std::unique_ptr<IRelayGrantStore> store_;
};

TEST_P(RelayGrantStoreContractTest, AReceivedGrantIsOnePerGrantorAndTheNewestWins) {
  EXPECT_FALSE(store_->Received("account:bob").value());
  ASSERT_TRUE(store_->PutReceived(Grant("account:bob", "account:me", 20)));
  ASSERT_TRUE(store_->PutReceived(Grant("account:bob", "account:me", 10))) << "an older one is ignored";
  EXPECT_EQ(store_->Received("account:bob").value(), Grant("account:bob", "account:me", 20));
  ASSERT_TRUE(store_->PutReceived(Grant("account:bob", "account:me", 30)));
  EXPECT_EQ(store_->Received("account:bob").value(), Grant("account:bob", "account:me", 30));
  ASSERT_TRUE(store_->DropReceived("account:bob"));
  EXPECT_FALSE(store_->Received("account:bob").value());
}

TEST_P(RelayGrantStoreContractTest, AnIssuedGrantIsOnePerGranteeWithItsConfirmation) {
  EXPECT_FALSE(store_->IssuedTo("account:bob").value());
  ASSERT_TRUE(store_->PutIssued({.grant = Grant("account:me", "account:bob", 10)}));
  ASSERT_TRUE(store_->PutIssued({.grant = Grant("account:me", "account:bob", 5), .confirmed = true}))
      << "what we gave last stands, newer or not (a renewal replaces it)";
  auto issued = store_->IssuedTo("account:bob").value();
  ASSERT_TRUE(issued);
  EXPECT_EQ(issued->grant, Grant("account:me", "account:bob", 5));
  EXPECT_TRUE(issued->confirmed);
  EXPECT_FALSE(store_->Received("account:bob").value()) << "issued and received are apart";
  ASSERT_TRUE(store_->DropIssued("account:bob"));
  EXPECT_FALSE(store_->IssuedTo("account:bob").value());
}

INSTANTIATE_TEST_SUITE_P(BothStores, RelayGrantStoreContractTest, ::testing::Values(Impl::Sqlite, Impl::Memory));

TEST(SqliteRelayGrantStoreTest, GrantsSurviveARestart) {
  const auto dir = std::filesystem::temp_directory_path() / ("pp_relay_grant_restart_" + util::GenerateUuid());
  std::filesystem::create_directories(dir);
  const std::string db = (dir / "profile.db").string();
  {
    auto store = std::make_unique<SqliteRelayGrantStore>(db);
    ASSERT_TRUE(store->PutReceived(Grant("account:bob", "account:me", 7)));
    ASSERT_TRUE(store->PutIssued({.grant = Grant("account:me", "account:bob", 8)}));
  }
  auto reopened = std::make_unique<SqliteRelayGrantStore>(db);
  EXPECT_EQ(reopened->Received("account:bob").value(), Grant("account:bob", "account:me", 7));
  EXPECT_EQ(reopened->IssuedTo("account:bob").value()->grant, Grant("account:me", "account:bob", 8));
  reopened.reset();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

} // namespace
} // namespace pbr
