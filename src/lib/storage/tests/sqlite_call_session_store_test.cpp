#include "lib/storage/SqliteCallSessionStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class CallSessionStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_session_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
    calls_ = std::make_unique<SqliteCallSessionStore>((data_dir_ / "profile.db").string());
  }

  void TearDown() override {
    calls_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteCallSessionStore> calls_;
};

/**
 * Zeroes `table`'s root page in the closed file `db_path` (never page 1, which holds the schema): a read of it is
 * still prepared, and its step fails — a real failed sqlite3_step, as a damaged page or an I/O error gives one.
 */
void ZeroTableRootPage(const std::filesystem::path& db_path, const std::string& table) {
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(db_path.string().c_str(), &db), SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_master WHERE type = 'table' AND name = ?;", -1, &stmt,
                               nullptr),
            SQLITE_OK);
  sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  const int64_t root = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA page_size;", -1, &stmt, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  const int64_t page_size = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  ASSERT_EQ(sqlite3_close(db), SQLITE_OK);
  ASSERT_GT(root, 1) << "page 1 is the schema";

  std::fstream file(db_path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.is_open());
  file.seekp(static_cast<std::streamoff>((root - 1) * page_size));
  const std::vector<char> zeros(static_cast<size_t>(page_size), 0);
  file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  ASSERT_TRUE(file.good());
}

CallSession ActiveSession(const std::string& call_id) {
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = "thread-1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = 1000;
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  return session;
}

CallParticipant Joined(const std::string& call_id, const std::string& identity) {
  CallParticipant participant;
  participant.call_id = call_id;
  participant.identity = identity;
  participant.state = CallParticipantState::Joined;
  participant.joined_at = 1000;
  return participant;
}

TEST_F(CallSessionStoreTest, UpsertSessionAndParticipants) {
  CallSession session;
  session.call_id = "call:test-1";
  session.origin_thread_id = "thread-1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Ringing;
  session.created_at = util::NowUnixMs();
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(calls_->UpsertSession(session));

  CallParticipant alice;
  alice.call_id = session.call_id;
  alice.identity = "account:alice";
  alice.state = CallParticipantState::Joined;
  alice.joined_at = session.created_at;
  ASSERT_TRUE(calls_->UpsertParticipant(alice));

  CallParticipant bob;
  bob.call_id = session.call_id;
  bob.identity = "account:bob";
  bob.state = CallParticipantState::Ringing;
  ASSERT_TRUE(calls_->UpsertParticipant(bob));

  auto loaded = calls_->LoadSession(session.call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->origin_thread_id, "thread-1");
  EXPECT_EQ((*loaded)->state, CallSessionState::Ringing);

  auto joined = calls_->CountJoined(session.call_id);
  ASSERT_TRUE(joined);
  EXPECT_EQ(*joined, 1u);

  PendingCallInvite invite;
  invite.call_id = session.call_id;
  invite.inviter_identity = "account:alice";
  invite.invitee_identity = "account:bob";
  invite.media_mode = CallMediaMode::Voice;
  invite.created_at = session.created_at;
  invite.expires_at = session.created_at + 60'000;
  invite.status = "pending";
  ASSERT_TRUE(calls_->UpsertPendingInvite(invite));

  auto pending = calls_->ListPendingInvitesForInvitee("account:bob");
  ASSERT_TRUE(pending);
  ASSERT_EQ(pending->size(), 1u);
  EXPECT_EQ(pending->front().call_id, session.call_id);

  ASSERT_TRUE(calls_->UpdateInviteStatus(session.call_id, "account:bob", "accepted"));
  auto after = calls_->LoadPendingInvite(session.call_id, "account:bob");
  ASSERT_TRUE(after && after->has_value());
  EXPECT_EQ((*after)->status, "accepted");
}

TEST_F(CallSessionStoreTest, PlannedHopPersists) {
  CallSession session;
  session.call_id = "call:planned";
  session.state = CallSessionState::Ringing;
  session.created_at = 1000;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(calls_->UpsertSession(session));
  auto loaded = calls_->LoadSession(session.call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_FALSE((*loaded)->planned_hop);

  session.planned_hop = CallPlannedHop{"12D3KooWHop", "/ip4/198.18.117.2/udp/443/adp/2/p2p/12D3KooWHop"};
  ASSERT_TRUE(calls_->UpsertSession(session));
  loaded = calls_->LoadSession(session.call_id);
  ASSERT_TRUE(loaded && loaded->has_value() && (*loaded)->planned_hop);
  EXPECT_EQ((*loaded)->planned_hop->peer_id, "12D3KooWHop");
  EXPECT_EQ((*loaded)->planned_hop->multiaddr, "/ip4/198.18.117.2/udp/443/adp/2/p2p/12D3KooWHop");
  auto active = calls_->ListActiveSessions();
  ASSERT_TRUE(active && !active->empty());
  ASSERT_TRUE(active->front().planned_hop);
}

TEST_F(CallSessionStoreTest, UpsertPreservesFirstJoinedAt) {
  CallSession session;
  session.call_id = "call:joined-at";
  session.origin_thread_id = "thread-1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = 1000;
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(calls_->UpsertSession(session));

  CallParticipant moto;
  moto.call_id = session.call_id;
  moto.identity = "account:moto";
  moto.state = CallParticipantState::Joined;
  moto.joined_at = 1000;
  ASSERT_TRUE(calls_->UpsertParticipant(moto));

  // CallRoster restamps with "now" on accept fan-out — must not steal SoftMigrate initiator.
  moto.joined_at = 9999;
  ASSERT_TRUE(calls_->UpsertParticipant(moto));

  auto parts = calls_->ListParticipants(session.call_id);
  ASSERT_TRUE(parts);
  ASSERT_EQ(parts->size(), 1u);
  ASSERT_TRUE(parts->front().joined_at);
  EXPECT_EQ(*parts->front().joined_at, 1000);
}

TEST_F(CallSessionStoreTest, JoiningAgainAfterLeavingStartsANewStay) {
  // B102 (#410): the first stay's stamp made the rejoin look stale to everyone who stayed.
  CallSession session;
  session.call_id = "call:rejoin-stamp";
  session.origin_thread_id = "thread-1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = 1000;
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(calls_->UpsertSession(session));

  CallParticipant guest;
  guest.call_id = session.call_id;
  guest.identity = "account:guest";
  guest.state = CallParticipantState::Joined;
  guest.joined_at = 1000;
  ASSERT_TRUE(calls_->UpsertParticipant(guest));
  guest.state = CallParticipantState::Left;
  guest.joined_at.reset();
  guest.left_at = 2000;
  ASSERT_TRUE(calls_->UpsertParticipant(guest));
  ASSERT_EQ(calls_->ListParticipants(session.call_id)->front().joined_at, std::optional<int64_t>(1000))
      << "a leave keeps the stay's stamp (a stale roster is judged against it)";

  guest.state = CallParticipantState::Joined;
  guest.joined_at = 3000;
  guest.left_at.reset();
  ASSERT_TRUE(calls_->UpsertParticipant(guest));
  EXPECT_EQ(calls_->ListParticipants(session.call_id)->front().joined_at, std::optional<int64_t>(3000));

  // Within the new stay the earliest stamp still wins (a later restamp on fan-out does not move it).
  guest.joined_at = 4000;
  ASSERT_TRUE(calls_->UpsertParticipant(guest));
  EXPECT_EQ(calls_->ListParticipants(session.call_id)->front().joined_at, std::optional<int64_t>(3000));
}

TEST_F(CallSessionStoreTest, UpsertTakesEarlierJoinedAtFromRoster) {
  CallSession session;
  session.call_id = "call:earlier-stamp";
  session.origin_thread_id = "thread-1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = 1000;
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(calls_->UpsertSession(session));

  CallParticipant owner;
  owner.call_id = session.call_id;
  owner.identity = "account:linux";
  owner.state = CallParticipantState::Joined;
  owner.joined_at = 5000; // wrong/late local stamp
  ASSERT_TRUE(calls_->UpsertParticipant(owner));

  owner.joined_at = 1000; // owner's CallRoster earliest stamp
  ASSERT_TRUE(calls_->UpsertParticipant(owner));

  auto parts = calls_->ListParticipants(session.call_id);
  ASSERT_TRUE(parts);
  ASSERT_TRUE(parts->front().joined_at);
  EXPECT_EQ(*parts->front().joined_at, 1000);
}

// DEV/PLAN-call-ui-never-detached.md 1: a read that fails part-way is an error, never the rows read before it — the call
// screen took "no rows" for "no call" while this device was still in one.
TEST_F(CallSessionStoreTest, AFailedStepOfTheParticipantsReadIsAnError) {
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:damaged")));
  ASSERT_TRUE(calls_->UpsertParticipant(Joined("call:damaged", "account:alice")));
  ASSERT_TRUE(calls_->UpsertParticipant(Joined("call:damaged", "account:bob")));
  ASSERT_TRUE(calls_->ListParticipants("call:damaged")) << "readable before the damage";
  ASSERT_NO_FATAL_FAILURE(ZeroTableRootPage(data_dir_ / "profile.db", "call_participants"));

  auto parts = calls_->ListParticipants("call:damaged");
  ASSERT_FALSE(parts) << "read " << parts->size() << " rows";
  EXPECT_NE(parts.error().message.find("ListParticipants"), std::string::npos) << parts.error().message;
  EXPECT_NE(parts.error().message.find("rc=11"), std::string::npos) << parts.error().message; // SQLITE_CORRUPT
  EXPECT_FALSE(calls_->FindParticipant("call:damaged", "account:alice"));
  EXPECT_FALSE(calls_->CountJoined("call:damaged"));
}

TEST_F(CallSessionStoreTest, AFailedStepOfTheActiveCallsReadIsAnError) {
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:damaged")));
  ASSERT_TRUE(calls_->ListActiveSessions()) << "readable before the damage";
  ASSERT_NO_FATAL_FAILURE(ZeroTableRootPage(data_dir_ / "profile.db", "call_sessions"));

  auto active = calls_->ListActiveSessions();
  ASSERT_FALSE(active) << "read " << active->size() << " rows";
  EXPECT_NE(active.error().message.find("ListActiveSessions"), std::string::npos) << active.error().message;
  EXPECT_NE(active.error().message.find("rc=11"), std::string::npos) << active.error().message; // SQLITE_CORRUPT
}

} // namespace
} // namespace pbr
