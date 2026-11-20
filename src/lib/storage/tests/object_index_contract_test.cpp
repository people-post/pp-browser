// Content's index port (CR024): one contract on the SQLite index and the in-memory one, and the SQLite one's rows
// surviving a restart.
#include "lib/content/testing/InMemoryObjectIndex.h"
#include "lib/storage/SqliteObjectIndex.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <memory>
#include <string>

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

int UserVersion(const std::filesystem::path& path) {
  sqlite3* db = nullptr;
  int version = -1;
  if (sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
      version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  sqlite3_close(db);
  return version;
}

void SetUserVersion(const std::filesystem::path& path, const int version) {
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(path.string().c_str(), &db), SQLITE_OK);
  const std::string sql = "PRAGMA user_version = " + std::to_string(version) + ";";
  EXPECT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(db);
}

// INVENTORY S3: object_index.db says its schema (`user_version` 1). A file from before it said one (0) is read as the
// current schema, its rows kept, and marked; a newer one is refused and left as it was.
TEST(SqliteObjectIndexTest, TheFileSaysItsVersionAndAnUnversionedOneIsTakenAsItIs) {
  const auto dir = std::filesystem::temp_directory_path() / ("pp_object_index_version_" + util::GenerateUuid());
  std::filesystem::create_directories(dir);
  const auto path = dir / "object_index.db";
  {
    SqliteObjectIndex index(dir.string());
    ASSERT_TRUE(index.Upsert(Meta(CasRealm::Private, 1, 100)));
  }
  EXPECT_EQ(UserVersion(path), SqliteObjectIndex::kSchemaVersion);
  SetUserVersion(path, 0);
  {
    SqliteObjectIndex reopened(dir.string());
    auto row = reopened.Lookup(CasRealm::Private, Id(1));
    ASSERT_TRUE(row) << row.error().message;
    EXPECT_TRUE(row->has_value()) << "its rows are kept";
  }
  EXPECT_EQ(UserVersion(path), SqliteObjectIndex::kSchemaVersion) << "and it says its version from now on";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(SqliteObjectIndexTest, ANewerFileIsRefusedAndLeftAlone) {
  const auto dir = std::filesystem::temp_directory_path() / ("pp_object_index_newer_" + util::GenerateUuid());
  std::filesystem::create_directories(dir);
  const auto path = dir / "object_index.db";
  {
    SqliteObjectIndex index(dir.string());
    ASSERT_TRUE(index.Upsert(Meta(CasRealm::Private, 1, 100)));
  }
  SetUserVersion(path, SqliteObjectIndex::kSchemaVersion + 1);
  {
    SqliteObjectIndex reopened(dir.string());
    auto row = reopened.Lookup(CasRealm::Private, Id(1));
    ASSERT_FALSE(row);
    EXPECT_NE(row.error().message.find("newer"), std::string::npos) << row.error().message;
    EXPECT_FALSE(reopened.Upsert(Meta(CasRealm::Private, 2, 200))) << "nothing is written to it";
  }
  EXPECT_EQ(UserVersion(path), SqliteObjectIndex::kSchemaVersion + 1);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

} // namespace
} // namespace pbr
