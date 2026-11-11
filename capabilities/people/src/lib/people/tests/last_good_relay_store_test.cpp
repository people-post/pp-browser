#include "lib/people/LastGoodRelayStore.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace pbr {
namespace {

using Relays = std::vector<std::string>;

class LastGoodRelayStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_browser_last_good_relays_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
  }
  void TearDown() override { std::filesystem::remove_all(data_dir_); }

  std::string Path() const { return (data_dir_ / "mesh_last_good_relays.json").string(); }
  void WriteFile(const std::string& body) const { std::ofstream(Path(), std::ios::binary) << body; }

  std::filesystem::path data_dir_;
};

// K015: the relay that carried the latest circuit leads; the list stays short and has no repeats.
TEST_F(LastGoodRelayStoreTest, TheLatestRelayLeadsAndTheListStaysShort) {
  LastGoodRelayStore store;
  EXPECT_TRUE(store.Note("QmA"));
  EXPECT_TRUE(store.Note("QmB"));
  EXPECT_EQ(store.Relays(), (Relays{"QmB", "QmA"}));
  EXPECT_FALSE(store.Note("QmB")) << "already first: nothing to save";
  EXPECT_TRUE(store.Note("QmA"));
  EXPECT_EQ(store.Relays(), (Relays{"QmA", "QmB"})) << "moved up, not repeated";
  EXPECT_TRUE(store.Note("QmC"));
  EXPECT_TRUE(store.Note("QmD"));
  EXPECT_EQ(store.Relays(), (Relays{"QmD", "QmC", "QmA"})) << "at most three: the oldest goes";
  EXPECT_FALSE(store.Note(""));
}

// K015: what a run learned is what the next one starts with.
TEST_F(LastGoodRelayStoreTest, ARestartReadsWhatTheLastRunSaved) {
  {
    LastGoodRelayStore store(Path());
    store.Note("QmOld");
    store.Note("QmNew");
    store.Save();
  }
  LastGoodRelayStore next(Path());
  EXPECT_TRUE(next.Relays().empty()) << "nothing before Load";
  next.Load();
  EXPECT_EQ(next.Relays(), (Relays{"QmNew", "QmOld"}));
}

TEST_F(LastGoodRelayStoreTest, AMissingOrForeignFileStartsEmpty) {
  LastGoodRelayStore missing(Path());
  missing.Load();
  EXPECT_TRUE(missing.Relays().empty());

  WriteFile(R"({"schema_version": 2, "relays": ["QmA"]})");
  LastGoodRelayStore newer(Path());
  newer.Load();
  EXPECT_TRUE(newer.Relays().empty()) << "another schema is ignored";

  WriteFile("not json");
  LastGoodRelayStore broken(Path());
  broken.Load();
  EXPECT_TRUE(broken.Relays().empty());
}

// A hand-edited or older file never yields more than the store keeps, nor repeats or blanks.
TEST_F(LastGoodRelayStoreTest, LoadKeepsTheBound) {
  WriteFile(R"({"schema_version": 1, "relays": ["QmA", "", "QmA", 7, "QmB", "QmC", "QmD"]})");
  LastGoodRelayStore store(Path());
  store.Load();
  EXPECT_EQ(store.Relays(), (Relays{"QmA", "QmB", "QmC"}));
}

} // namespace
} // namespace pbr
