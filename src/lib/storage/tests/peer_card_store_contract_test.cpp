// People's card store port (CR024): one contract on the SQLite store and the in-memory one, and the SQLite one's
// rows surviving a restart.
#include "lib/people/testing/InMemoryPeerCards.h"
#include "lib/storage/SqlitePeerCardStore.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>

namespace pbr {
namespace {

PeerCardRecord Record(const std::string& account_id, int64_t issued_at, const std::string& nickname) {
  PeerCardRecord record;
  record.keys.account_id = account_id; // shape only: the store keeps what PeerCards checked
  record.keys.signing_public_key_b64 = "c2lnbmluZw==";
  record.keys.kem_public_key_b64 = "a2Vt";
  record.keys.kem_binding_signature_b64 = "YmluZGluZw==";
  record.issued_at = issued_at;
  record.nickname = nickname;
  record.devices = {"QmPhone"};
  record.source = "envelope";
  return record;
}

enum class Impl { Sqlite, Memory };

class PeerCardStoreContractTest : public ::testing::TestWithParam<Impl> {
protected:
  void SetUp() override {
    if (GetParam() == Impl::Sqlite) {
      dir_ = std::filesystem::temp_directory_path() / ("pp_peer_card_contract_" + util::GenerateUuid());
      std::filesystem::create_directories(dir_);
      store_ = std::make_unique<SqlitePeerCardStore>(Db());
    } else {
      store_ = std::make_unique<InMemoryPeerCardStore>();
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
  std::unique_ptr<IPeerCardStore> store_;
};

TEST_P(PeerCardStoreContractTest, ARecordIsOnePerAccountAndReadsBackWhole) {
  EXPECT_FALSE(store_->Get("account:alice").value().has_value());
  ASSERT_TRUE(store_->Put(Record("account:alice", 10, "Alice")));
  ASSERT_TRUE(store_->Put(Record("account:alice", 20, "Ali")));
  ASSERT_TRUE(store_->Put(Record("account:bob", 5, "Bob")));
  auto alice = store_->Get("account:alice").value();
  ASSERT_TRUE(alice);
  EXPECT_EQ(alice->issued_at, 20);
  EXPECT_EQ(alice->nickname, "Ali");
  EXPECT_EQ(alice->devices, std::vector<std::string>{"QmPhone"});
  EXPECT_EQ(store_->List().value().size(), 2u);
}

TEST_P(PeerCardStoreContractTest, FloorsOnlyRise) {
  EXPECT_EQ(store_->Floor("account:alice").value(), 0);
  ASSERT_TRUE(store_->RaiseFloors({{"account:alice", 50}, {"account:bob", 7}}));
  ASSERT_TRUE(store_->RaiseFloors({{"account:alice", 40}}));
  EXPECT_EQ(store_->Floor("account:alice").value(), 50);
  auto floors = store_->Floors().value();
  std::sort(floors.begin(), floors.end(),
            [](const PeerCardFloor& a, const PeerCardFloor& b) { return a.account_id < b.account_id; });
  EXPECT_EQ(floors, (std::vector<PeerCardFloor>{{"account:alice", 50}, {"account:bob", 7}}));
}

INSTANTIATE_TEST_SUITE_P(Impls, PeerCardStoreContractTest, ::testing::Values(Impl::Sqlite, Impl::Memory),
                         [](const ::testing::TestParamInfo<Impl>& info) {
                           return info.param == Impl::Sqlite ? std::string("Sqlite") : std::string("Memory");
                         });

TEST(SqlitePeerCardStoreTest, RecordsAndFloorsSurviveARestart) {
  const auto dir = std::filesystem::temp_directory_path() / ("pp_peer_card_restart_" + util::GenerateUuid());
  std::filesystem::create_directories(dir);
  const std::string db = (dir / "profile.db").string();
  {
    SqlitePeerCardStore store(db);
    ASSERT_TRUE(store.Put(Record("account:alice", 10, "Alice")));
    ASSERT_TRUE(store.RaiseFloors({{"account:alice", 12}}));
  }
  {
    SqlitePeerCardStore reopened(db);
    EXPECT_EQ(reopened.Get("account:alice").value()->nickname, "Alice");
    EXPECT_EQ(reopened.Floor("account:alice").value(), 12);
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

} // namespace
} // namespace pbr
