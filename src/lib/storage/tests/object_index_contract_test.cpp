// Content's index port (CR024): one contract on the SQLite index and the in-memory one, and the SQLite one's rows
// surviving a restart.
#include "lib/content/testing/InMemoryObjectIndex.h"
#include "lib/storage/SqliteObjectIndex.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace pbr {
namespace {

ByteVector Id(const uint8_t fill) {
  return ByteVector(kCasContentIdSize, fill);
}

CasObjectMeta Meta(const CasRealm realm, const uint8_t fill, const int64_t created_at_ms, const bool pinned = true) {
  CasObjectMeta meta;
  meta.realm = realm;
  meta.content_id = Id(fill);
  meta.mime = "image/png";
  meta.filename = "photo.png";
  meta.byte_length = 1234;
  meta.pinned = pinned;
  meta.created_at_ms = created_at_ms;
  return meta;
}

enum class Impl { Sqlite, Memory };

class ObjectIndexContractTest : public ::testing::TestWithParam<Impl> {
protected:
  void SetUp() override {
    if (GetParam() == Impl::Sqlite) {
      dir_ = std::filesystem::temp_directory_path() / ("pp_object_index_contract_" + util::GenerateUuid());
      std::filesystem::create_directories(dir_);
      index_ = std::make_unique<SqliteObjectIndex>(dir_.string());
    } else {
      index_ = std::make_unique<InMemoryObjectIndex>();
    }
  }
  void TearDown() override {
    index_.reset();
    if (!dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
    }
  }

  std::filesystem::path dir_;
  std::unique_ptr<IObjectIndex> index_;
};

TEST_P(ObjectIndexContractTest, ARowIsOnePerRealmAndIdAndReadsBackWhole) {
  EXPECT_FALSE(index_->Lookup(CasRealm::Private, Id(1)).value().has_value());
  CasObjectMeta meta = Meta(CasRealm::Private, 1, 100);
  meta.published_from_hex = "ab";
  ASSERT_TRUE(index_->Upsert(meta));
  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Public, 1, 200)));
  auto row = index_->Lookup(CasRealm::Private, Id(1)).value();
  ASSERT_TRUE(row);
  EXPECT_EQ(row->mime, "image/png");
  EXPECT_EQ(row->filename, "photo.png");
  EXPECT_EQ(row->byte_length, 1234u);
  EXPECT_EQ(row->published_from_hex, "ab");
  EXPECT_TRUE(row->pinned);
  EXPECT_EQ(row->created_at_ms, 100);
  EXPECT_EQ(index_->List(std::nullopt, std::nullopt).value().size(), 2u);
}

TEST_P(ObjectIndexContractTest, AnUpdateKeepsTheRowsTimeAndANewRowGetsOne) {
  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Private, 1, 100)));
  CasObjectMeta again = Meta(CasRealm::Private, 1, 999);
  again.filename = "renamed.png";
  ASSERT_TRUE(index_->Upsert(again));
  auto row = index_->Lookup(CasRealm::Private, Id(1)).value();
  EXPECT_EQ(row->filename, "renamed.png");
  EXPECT_EQ(row->created_at_ms, 100);

  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Private, 2, 0)));
  EXPECT_GT(index_->Lookup(CasRealm::Private, Id(2)).value()->created_at_ms, 0);
}

TEST_P(ObjectIndexContractTest, ListFiltersAndIsNewestFirstThenById) {
  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Private, 3, 100)));
  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Private, 2, 100)));
  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Private, 1, 50, /*pinned=*/false)));
  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Public, 4, 300)));

  auto all = index_->List(std::nullopt, std::nullopt).value();
  ASSERT_EQ(all.size(), 4u);
  EXPECT_EQ(all[0].content_id, Id(4));
  EXPECT_EQ(all[1].content_id, Id(2)); // same time: by id
  EXPECT_EQ(all[2].content_id, Id(3));
  EXPECT_EQ(all[3].content_id, Id(1));

  EXPECT_EQ(index_->List(CasRealm::Private, std::nullopt).value().size(), 3u);
  auto cache = index_->List(std::nullopt, false).value();
  ASSERT_EQ(cache.size(), 1u);
  EXPECT_EQ(cache[0].content_id, Id(1));
  EXPECT_EQ(index_->List(CasRealm::Public, true).value().size(), 1u);
}

TEST_P(ObjectIndexContractTest, PinAndRemoveTouchOnlyTheirRow) {
  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Public, 1, 100)));
  ASSERT_TRUE(index_->Upsert(Meta(CasRealm::Private, 1, 100)));
  ASSERT_TRUE(index_->SetPinned(CasRealm::Public, Id(1), false));
  EXPECT_FALSE(index_->Lookup(CasRealm::Public, Id(1)).value()->pinned);
  EXPECT_TRUE(index_->Lookup(CasRealm::Private, Id(1)).value()->pinned);

  ASSERT_TRUE(index_->SetPinned(CasRealm::Public, Id(9), true)) << "a missing row is not an error";
  EXPECT_FALSE(index_->Lookup(CasRealm::Public, Id(9)).value().has_value()) << "and none is added";

  ASSERT_TRUE(index_->Remove(CasRealm::Public, Id(1)));
  ASSERT_TRUE(index_->Remove(CasRealm::Public, Id(1))) << "removing a missing row is not an error";
  EXPECT_FALSE(index_->Lookup(CasRealm::Public, Id(1)).value().has_value());
  EXPECT_TRUE(index_->Lookup(CasRealm::Private, Id(1)).value().has_value());
}

TEST_P(ObjectIndexContractTest, ABadContentIdIsRefused) {
  const ByteVector short_id(4, 0x1);
  CasObjectMeta meta = Meta(CasRealm::Private, 1, 100);
  meta.content_id = short_id;
  EXPECT_FALSE(index_->Upsert(meta));
  EXPECT_FALSE(index_->Lookup(CasRealm::Private, short_id));
  EXPECT_FALSE(index_->Remove(CasRealm::Private, short_id));
  EXPECT_FALSE(index_->SetPinned(CasRealm::Private, short_id, false));
}

INSTANTIATE_TEST_SUITE_P(Impls, ObjectIndexContractTest, ::testing::Values(Impl::Sqlite, Impl::Memory),
                         [](const ::testing::TestParamInfo<Impl>& info) {
                           return info.param == Impl::Sqlite ? std::string("Sqlite") : std::string("Memory");
                         });

TEST(SqliteObjectIndexTest, RowsSurviveARestart) {
  const auto dir = std::filesystem::temp_directory_path() / ("pp_object_index_restart_" + util::GenerateUuid());
  std::filesystem::create_directories(dir);
  {
    SqliteObjectIndex index(dir.string());
    ASSERT_TRUE(index.Upsert(Meta(CasRealm::Private, 1, 100, /*pinned=*/false)));
  }
  EXPECT_TRUE(std::filesystem::exists(dir / "object_index.db")) << "its own file beside the profile database";
  {
    SqliteObjectIndex reopened(dir.string());
    auto row = reopened.Lookup(CasRealm::Private, Id(1)).value();
    ASSERT_TRUE(row);
    EXPECT_FALSE(row->pinned);
    EXPECT_EQ(row->created_at_ms, 100);
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

} // namespace
} // namespace pbr
