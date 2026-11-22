#include "lib/net/RelayInboxCursor.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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
  RelayInboxCursors cursors{.preferred = "p1",
                            .others = {{"https://b.example/api/relay", "b1"}},
                            .registrations = std::map<std::string, std::string>{}};
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

// The registration each cursor counts in (a fresh registration is a new mailbox) is kept beside the cursors, additively:
// the version stays 1, the cursors stay where an older app reads them, and a file without it (an older app's) says so.
// It is written even empty: its presence says the cursors were kept by an app that notes their mailbox.
TEST_F(RelayInboxCursorTest, TheRegistrationEachCursorCountsInComesBackBesideTheCursors) {
  const RelayInboxCursors cursors{
      .preferred = "c1:7",
      .preferred_relay = "https://a.example/api/relay",
      .others = {{"https://b.example/api/relay", "c1:3"}},
      .registrations = std::map<std::string, std::string>{{"https://a.example/api/relay", "reg-a"},
                                                          {"https://b.example/api/relay", "reg-b"}}};
  SaveRelayInboxCursors(dir_.string(), "account:me", cursors);
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), "account:me"), cursors);

  std::ifstream in(dir_ / "relay_inbox_cursor.json");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("\"schema_version\":1"), std::string::npos) << text;
  EXPECT_NE(text.find("\"cursor\":\"c1:7\""), std::string::npos) << "where an older app reads it: " << text;

  std::ofstream(dir_ / "relay_inbox_cursor.json", std::ios::trunc)
      << R"({"schema_version":1,"account_id":"account:me","cursor":"c1:1684"})";
  EXPECT_FALSE(LoadRelayInboxCursors(dir_.string(), "account:me").registrations) << "a file from before it";

  SaveRelayInboxCursors(dir_.string(), "account:me", {.preferred = "c1:2", .others = {}});
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), "account:me").registrations, (std::map<std::string, std::string>{}))
      << "written empty too";
}

// INVENTORY S3: the file says its schema from now on (a file without one, as above, is today's); a newer app's is not
// read — that inbox is fetched from its oldest message, which the receive pipeline drops as seen — and never written.
TEST_F(RelayInboxCursorTest, TheFileSaysItsVersionAndANewerOneIsLeftAlone) {
  const auto path = dir_ / "relay_inbox_cursor.json";
  const auto read = [&path]() {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  SaveRelayInboxCursors(dir_.string(), "account:me", {.preferred = "p1", .others = {}});
  EXPECT_NE(read().find("\"schema_version\":1"), std::string::npos) << read();

  const std::string newer = R"({"schema_version":2,"account_id":"account:me","cursor":"p9"})";
  std::ofstream(path, std::ios::trunc) << newer;
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), "account:me"), RelayInboxCursors{});
  SaveRelayInboxCursors(dir_.string(), "account:me", {.preferred = "p2", .others = {}});
  EXPECT_EQ(read(), newer) << "never written over";
}

} // namespace
} // namespace pbr
