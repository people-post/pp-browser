#include "lib/net/RelayInboxCursor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
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

  std::string text;
  { // closed before the file is replaced below: an open file cannot be replaced on Windows
    std::ifstream in(dir_ / "relay_inbox_cursor.json");
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
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

/** The file as the app before `registrations` / `reread_*` read it (its LoadRelayInboxCursors, verbatim). */
RelayInboxCursors ReadAsAnOlderApp(const std::string& text, const std::string& account_id) {
  RelayInboxCursors cursors;
  const auto root = TryParseObject(text);
  if (!root || root->getNonNegInt("schema_version").value_or(1) > 1 || root->getString("account_id") != account_id) {
    return cursors;
  }
  cursors.preferred = root->getString("cursor").value_or("");
  cursors.preferred_relay = root->getString("cursor_relay").value_or("");
  if (const Object* relays = root->getObject("relays")) {
    for (const auto& [origin, value] : relays->entries()) {
      if (auto cursor = asString(value); cursor && !origin.empty() && !cursor->empty()) {
        cursors.others[origin] = *cursor;
      }
    }
  }
  return cursors;
}

// An older app's file's re-read under way (RelayInboxCoordinator: its phase, and phase 2's floor when there is one) is
// kept beside the cursors, additively: the version stays 1 and an older app reads the cursors as before; nothing is
// written when there is none; a phase this app does not know, or a floor alone, is no re-read.
TEST_F(RelayInboxCursorTest, AReReadUnderWayComesBackAndAnOlderAppReadsTheFileAsBefore) {
  const RelayInboxCursors cursors{
      .preferred = "c1:7",
      .preferred_relay = "https://a.example/api/relay",
      .others = {{"https://b.example/api/relay", "c1:3"}, {"https://c.example/api/relay", "c1:9"}},
      .registrations = std::map<std::string, std::string>{},
      .rereads = {{"https://a.example/api/relay", {.backfill = true, .floor_ms = 1'700'000'000'000}},
                  {"https://b.example/api/relay", {.backfill = false, .floor_ms = 1'700'000'000'000}},
                  {"https://c.example/api/relay", {.backfill = true, .floor_ms = std::nullopt}}}};
  SaveRelayInboxCursors(dir_.string(), "account:me", cursors);
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), "account:me"), cursors);

  const auto read = [this]() {
    std::ifstream in(dir_ / "relay_inbox_cursor.json");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const std::string text = read();
  EXPECT_NE(text.find("\"schema_version\":1"), std::string::npos) << text;
  EXPECT_NE(text.find("\"https://a.example/api/relay\":\"backfill\""), std::string::npos) << text;
  EXPECT_NE(text.find("\"https://b.example/api/relay\":\"drain\""), std::string::npos) << text;
  EXPECT_NE(text.find("\"reread_floor_ms\":{\"https://a.example/api/relay\":1700000000000"), std::string::npos) << text;
  EXPECT_EQ(
      ReadAsAnOlderApp(text, "account:me"),
      (RelayInboxCursors{.preferred = "c1:7",
                         .preferred_relay = "https://a.example/api/relay",
                         .others = {{"https://b.example/api/relay", "c1:3"}, {"https://c.example/api/relay", "c1:9"}}}))
      << "an older app ignores the keys";

  SaveRelayInboxCursors(dir_.string(), "account:me", {.preferred = "c1:8", .others = {}});
  EXPECT_EQ(read().find("reread"), std::string::npos) << "none: not written";

  { // closed before it is read: Windows
    std::ofstream out(dir_ / "relay_inbox_cursor.json", std::ios::trunc);
    out << R"({"schema_version":1,"account_id":"account:me","cursor":"c1:8","registrations":{},)"
        << R"("reread_phase":{"https://a.example/api/relay":"later"},)"
        << R"("reread_floor_ms":{"https://a.example/api/relay":5,"https://b.example/api/relay":5}})";
  }
  EXPECT_TRUE(LoadRelayInboxCursors(dir_.string(), "account:me").rereads.empty());
}

// The file's last write, on the wall clock (the relay's store times are Unix ms): none without a file.
TEST_F(RelayInboxCursorTest, TheFilesLastWriteIsReadOnTheWallClock) {
  EXPECT_FALSE(RelayInboxCursorFileWrittenMs(dir_.string()));
  EXPECT_FALSE(RelayInboxCursorFileWrittenMs(""));
  { // closed before its time is set: Windows
    std::ofstream(dir_ / "relay_inbox_cursor.json") << R"({"account_id":"account:me","cursor":"c1:1"})";
  }
  std::filesystem::last_write_time(dir_ / "relay_inbox_cursor.json",
                                   std::filesystem::file_time_type::clock::now() - std::chrono::hours(3));
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const auto written = RelayInboxCursorFileWrittenMs(dir_.string());
  ASSERT_TRUE(written);
  EXPECT_NEAR(static_cast<double>(*written), static_cast<double>(now_ms - 3 * 3600 * 1000), 60'000.0);
}

// A file time at either end of what the file clock holds (a copied or corrupt profile) converts without overflow: it is
// none, or a time the coordinator does not trust (in the future, or before 1970 plus its slack).
TEST_F(RelayInboxCursorTest, AnExtremeFileTimeIsReadWithoutOverflow) {
  const auto path = dir_ / "relay_inbox_cursor.json";
  { // closed before its time is set: Windows
    std::ofstream(path) << R"({"account_id":"account:me","cursor":"c1:1"})";
  }
  using FileTime = std::filesystem::file_time_type;
  for (const FileTime extreme : {FileTime::min(), FileTime::max(), FileTime{}}) {
    std::error_code ec;
    std::filesystem::last_write_time(path, extreme, ec);
    if (ec) {
      continue; // this file system refuses that time: nothing to read back
    }
    const auto written = RelayInboxCursorFileWrittenMs(dir_.string()); // must not trap or overflow
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (written) {
      EXPECT_TRUE(*written > now_ms || *written < now_ms - 24LL * 3600 * 1000 * 365)
          << "an extreme time reads as an extreme one: " << *written;
    }
  }
}

} // namespace
} // namespace pbr
