// What the SQLite roster adds to the port's contract (messaging_store_contract_test): ending a group is one
// transaction, what a dissolve still owes survives a restart (G016), and accepts over many connections all land.
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/storage/SqliteGroupRosterStore.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace pbr {
namespace {

class SqliteGroupRosterStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_sqlite_roster_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    rosters_ = std::make_unique<SqliteGroupRosterStore>(Db());
    GroupMetadata metadata;
    metadata.group_id = "group:g";
    metadata.owner_identity = "account:me";
    metadata.roster_epoch = 3;
    ASSERT_TRUE(rosters_->UpsertMetadata(metadata));
    ASSERT_TRUE(rosters_->UpsertMember("group:g", {"account:bob", "contact:bob", MemberRole::Member, 1}));
  }
  void TearDown() override {
    rosters_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string Db() const { return (dir_ / "profile.db").string(); }

  static GroupEnding Ending() {
    GroupEnding ending;
    ending.owner_identity = "account:me";
    ending.roster_epoch = 4;
    ending.statement_json = "stmt";
    ending.sends.push_back({"group:g", "account:bob", "group_dissolved", "{}", "Bob"});
    return ending;
  }

  std::filesystem::path dir_;
  std::unique_ptr<SqliteGroupRosterStore> rosters_;
};

TEST_F(SqliteGroupRosterStoreTest, AFailurePartWayLeavesTheGroupAsItWas) {
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(Db().c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db,
                         "CREATE TRIGGER fail_send BEFORE INSERT ON group_pending_sends BEGIN "
                         "SELECT RAISE(ABORT, 'boom'); END;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  EXPECT_FALSE(rosters_->EndGroup("group:g", Ending()));
  EXPECT_EQ(rosters_->LoadMetadata("group:g").value()->sync_state, GroupSyncState::Active);
  EXPECT_EQ(rosters_->ListMembers("group:g").value().size(), 1u);
  EXPECT_TRUE(rosters_->ListPendingSends().value().empty());

  ASSERT_EQ(sqlite3_exec(db, "DROP TRIGGER fail_send;", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(db);
  auto ended = rosters_->EndGroup("group:g", Ending());
  ASSERT_TRUE(ended && ended->has_value());
  EXPECT_EQ(rosters_->LoadMetadata("group:g").value()->sync_state, GroupSyncState::Dissolved);
}

TEST_F(SqliteGroupRosterStoreTest, WhatADissolveStillOwesSurvivesARestart) {
  ASSERT_TRUE(rosters_->EndGroup("group:g", Ending()));
  rosters_.reset();
  SqliteGroupRosterStore reopened(Db());
  auto pending = reopened.ListPendingSends("group:g").value();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].recipient, "account:bob");
  EXPECT_EQ(reopened.LoadMetadata("group:g").value()->sync_state, GroupSyncState::Dissolved);
}

TEST_F(SqliteGroupRosterStoreTest, ConcurrentAcceptsOverTheirOwnConnectionsAllLand) {
  constexpr int kInvitees = 8;
  for (int i = 0; i < kInvitees; ++i) {
    PendingGroupInvite pending;
    pending.invite_nonce = "nonce-" + std::to_string(i);
    pending.group_id = "group:g";
    pending.inviter_identity = "account:me";
    pending.invitee_identity = "account:member" + std::to_string(i);
    pending.created_at = util::NowUnixMs();
    pending.outbound = true;
    ASSERT_TRUE(rosters_->UpsertPendingInvite(pending));
  }
  std::atomic<int> applied{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < kInvitees; ++i) {
    workers.emplace_back([this, i, &applied]() {
      SqliteGroupRosterStore own(Db()); // each worker its own connection, as in the app
      auto accepted = ApplyInviteAcceptToRoster(own, "nonce-" + std::to_string(i), "account:member" + std::to_string(i),
                                                "group:g", "account:me", util::NowUnixMs());
      if (accepted && *accepted == InviteAccepted::Added) {
        applied.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(applied.load(), kInvitees);
  EXPECT_EQ(rosters_->LoadMetadata("group:g").value()->roster_epoch, 3u + kInvitees) << "every accept moved the epoch";
}

} // namespace
} // namespace pbr
