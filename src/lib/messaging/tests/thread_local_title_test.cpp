// A thread's local title survives a reopen (the store migrates its column).
#include "lib/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(SqliteThreadStoreLocalTitleTest, MigratesAndPersistsLocalTitle) {
  const auto data_dir = std::filesystem::temp_directory_path() / ("pp_browser_local_title_migrate_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::remove_all(data_dir);

  {
    SqliteThreadStore store(data_dir.string());
    ASSERT_TRUE(store.ListThreads());
    Thread thread;
    thread.id = "t1";
    thread.kind = ThreadKind::Group;
    thread.group_id = "group:x";
    thread.title = "Shared";
    thread.local_title = "Mine";
    ASSERT_TRUE(store.UpsertThread(thread));
  }

  {
    // See "Temp-dir cleanup pattern" above — reloaded must close before remove_all.
    SqliteThreadStore reloaded(data_dir.string());
    auto loaded = reloaded.GetThread("t1");
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(*loaded);
    EXPECT_EQ((*loaded)->title, "Shared");
    EXPECT_EQ((*loaded)->local_title, "Mine");
  }

  std::filesystem::remove_all(data_dir);
}

} // namespace
} // namespace pbr
