#include "lib/net/RelayInboxCursor.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Each of our relay inboxes keeps its own poll cursor (multi-relay R2; DATA_LAYOUT `relay_inbox_cursor.json`): the
// preferred relay's under `cursor`, the others' under `relays` by origin.

class RelayInboxCursorTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp-relay-cursor-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(RelayInboxCursorTest, EachRelaysCursorComesBack) {
  RelayInboxCursors cursors{.preferred = "p1", .others = {{"https://b.example/api/relay", "b1"}}};
  SaveRelayInboxCursors(dir_.string(), "account:me", cursors);
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), "account:me"), cursors);
}

TEST_F(RelayInboxCursorTest, AnotherAccountsFileIsNone) {
  SaveRelayInboxCursors(dir_.string(), "account:me", {.preferred = "p1", .others = {}});
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), "account:other"), RelayInboxCursors{});
}

TEST_F(RelayInboxCursorTest, AFileWithThePreferredCursorOnlyReadsAsBefore) {
  std::ofstream(dir_ / "relay_inbox_cursor.json") << R"({"account_id":"account:me","cursor":"p1"})";
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), "account:me"), (RelayInboxCursors{.preferred = "p1", .others = {}}));
}

} // namespace
} // namespace pbr
