#include "lib/storage/SqliteCallSessionStore.h"
#include "common/Utilities.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <sqlite3.h>
#include <thread>
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

  session.planned_hop = CallPlannedHop{"12D3KooWHop", "/ip4/198.18.117.2/udp/443/adp/1.0.0/p2p/12D3KooWHop"};
  ASSERT_TRUE(calls_->UpsertSession(session));
  loaded = calls_->LoadSession(session.call_id);
  ASSERT_TRUE(loaded && loaded->has_value() && (*loaded)->planned_hop);
  EXPECT_EQ((*loaded)->planned_hop->peer_id, "12D3KooWHop");
  EXPECT_EQ((*loaded)->planned_hop->multiaddr, "/ip4/198.18.117.2/udp/443/adp/1.0.0/p2p/12D3KooWHop");
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
  // A live store keeps its read connection (and the pages it has read): damage the closed file, read with a new store.
  calls_.reset();
  ASSERT_NO_FATAL_FAILURE(ZeroTableRootPage(data_dir_ / "profile.db", "call_participants"));
  calls_ = std::make_unique<SqliteCallSessionStore>((data_dir_ / "profile.db").string());

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
  calls_.reset();
  ASSERT_NO_FATAL_FAILURE(ZeroTableRootPage(data_dir_ / "profile.db", "call_sessions"));
  calls_ = std::make_unique<SqliteCallSessionStore>((data_dir_ / "profile.db").string());

  auto active = calls_->ListActiveSessions();
  ASSERT_FALSE(active) << "read " << active->size() << " rows";
  EXPECT_NE(active.error().message.find("ListActiveSessions"), std::string::npos) << active.error().message;
  EXPECT_NE(active.error().message.find("rc=11"), std::string::npos) << active.error().message; // SQLITE_CORRUPT
}

// DEV/PLAN-call-frame-cost.md step 5: the reads share one read-only connection kept open (a call read the store seven
// times a frame, each time opening profile.db again); the writes still open their own. profile.db is WAL in the app.

/** Puts the (not yet created) file `db_path` in WAL mode, as SqliteThreadDb has profile.db. */
void UseWal(const std::filesystem::path& db_path) {
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(db_path.string().c_str(), &db), SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL;", -1, &stmt, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  const std::string mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  ASSERT_EQ(sqlite3_close(db), SQLITE_OK);
  ASSERT_EQ(mode, "wal");
}

/** Another connection on the same file (SqliteThreadDb's, a short-lived store's). */
class OtherConnection {
public:
  explicit OtherConnection(const std::filesystem::path& db_path) {
    EXPECT_EQ(sqlite3_open(db_path.string().c_str(), &db_), SQLITE_OK);
    sqlite3_busy_timeout(db_, 5000);
  }
  OtherConnection(const OtherConnection&) = delete;
  OtherConnection& operator=(const OtherConnection&) = delete;
  ~OtherConnection() { sqlite3_close(db_); }
  int Exec(const std::string& sql) { return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr); }

private:
  sqlite3* db_ = nullptr;
};

PendingCallInvite Invite(const std::string& call_id, const std::string& invitee) {
  PendingCallInvite invite;
  invite.call_id = call_id;
  invite.inviter_identity = "account:alice";
  invite.invitee_identity = invitee;
  invite.created_at = 1000;
  invite.status = "pending";
  return invite;
}

/** Waits for `finished` to reach `count`, then joins; a deadlock never gets there, so the binary ends instead. */
void JoinWithin(std::vector<std::thread>& threads, const std::atomic<int>& finished, const std::chrono::seconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (finished.load() < static_cast<int>(threads.size())) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "threads not done after %lld s: deadlock\n", static_cast<long long>(limit.count()));
      std::abort();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

TEST_F(CallSessionStoreTest, ReadsShareOneConnectionOpenedOnce) {
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:x")));
  ASSERT_TRUE(calls_->UpsertParticipant(Joined("call:x", "account:alice")));
  ASSERT_TRUE(calls_->UpsertPendingInvite(Invite("call:x", "account:me")));
  EXPECT_EQ(calls_->ReadConnectionOpensForTest(), 0u) << "writes do not open the read connection";
  for (int i = 0; i < 1000; ++i) {
    switch (i % 9) {
      case 0:
        ASSERT_TRUE(calls_->LoadSession("call:x").value().has_value());
        break;
      case 1:
        ASSERT_EQ(calls_->ListActiveSessions().value().size(), 1u);
        break;
      case 2:
        ASSERT_TRUE(calls_->ListSessionsForGroup("group:none"));
        break;
      case 3:
        ASSERT_EQ(calls_->ListRecentSessions(5).value().size(), 1u);
        break;
      case 4:
        ASSERT_EQ(calls_->ListParticipants("call:x").value().size(), 1u);
        break;
      case 5:
        ASSERT_TRUE(calls_->FindParticipant("call:x", "account:alice").value().has_value());
        break;
      case 6:
        ASSERT_EQ(calls_->CountJoined("call:x").value(), 1u);
        break;
      case 7:
        ASSERT_TRUE(calls_->LoadPendingInvite("call:x", "account:me").value().has_value());
        break;
      default:
        ASSERT_EQ(calls_->ListPendingInvitesForInvitee("account:me").value().size(), 1u);
        break;
    }
    if (i % 100 == 0) {
      ASSERT_TRUE(calls_->UpsertParticipant(Joined("call:x", "account:alice")));
    }
  }
  EXPECT_EQ(calls_->ReadConnectionOpensForTest(), 1u);
}

// A read stopped on its one row (LoadSession, LoadPendingInvite) and not finalized would keep the connection on that
// snapshot: every later read would miss what others wrote.
TEST_F(CallSessionStoreTest, AWriteCommittedElsewhereIsSeenByTheNextRead) {
  ASSERT_NO_FATAL_FAILURE(UseWal(data_dir_ / "profile.db"));
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:x")));
  ASSERT_TRUE(calls_->UpsertPendingInvite(Invite("call:x", "account:me")));
  ASSERT_EQ(calls_->LoadSession("call:x").value()->state, CallSessionState::Active);
  ASSERT_EQ(calls_->LoadPendingInvite("call:x", "account:me").value()->status, "pending");

  OtherConnection other(data_dir_ / "profile.db");
  ASSERT_EQ(other.Exec("UPDATE call_sessions SET state = 'ended' WHERE call_id = 'call:x';"), SQLITE_OK);
  EXPECT_EQ(calls_->LoadSession("call:x").value()->state, CallSessionState::Ended);
  EXPECT_TRUE(calls_->ListActiveSessions().value().empty());
  ASSERT_EQ(other.Exec("UPDATE pending_call_invites SET status = 'accepted' WHERE call_id = 'call:x';"), SQLITE_OK);
  EXPECT_EQ(calls_->LoadPendingInvite("call:x", "account:me").value()->status, "accepted");
  EXPECT_TRUE(calls_->ListPendingInvitesForInvitee("account:me").value().empty());

  {
    // A short-lived store (Application's recent-calls reader, the demo seeder) writing on its own.
    const SqliteCallSessionStore temporary((data_dir_ / "profile.db").string());
    ASSERT_TRUE(temporary.UpsertParticipant(Joined("call:x", "account:carol")));
  }
  EXPECT_EQ(calls_->ListParticipants("call:x").value().size(), 1u);
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:y")));
  EXPECT_EQ(calls_->ListActiveSessions().value().size(), 1u) << "this store's own write";
  EXPECT_EQ(calls_->ReadConnectionOpensForTest(), 1u);
}

// The UI thread reads every frame: a writer holding its lock (uncommitted) elsewhere must not make it wait.
TEST_F(CallSessionStoreTest, AnOpenWriteTransactionElsewhereNeitherHoldsUpAReadNorShowsInIt) {
  ASSERT_NO_FATAL_FAILURE(UseWal(data_dir_ / "profile.db"));
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:before")));
  ASSERT_EQ(calls_->ListActiveSessions().value().size(), 1u);

  OtherConnection writer(data_dir_ / "profile.db");
  ASSERT_EQ(writer.Exec("BEGIN IMMEDIATE;"), SQLITE_OK);
  ASSERT_EQ(writer.Exec("INSERT INTO call_sessions (call_id, state, created_at) VALUES ('call:new', 'active', 2000);"),
            SQLITE_OK);
  ASSERT_EQ(writer.Exec("UPDATE call_sessions SET state = 'ended' WHERE call_id = 'call:before';"), SQLITE_OK);

  // The shared connection answers each read without a busy wait or a reopen. Measured on the Mac: ~10 us a pair of
  // reads on the shared connection, ~220 us with a store (so a connection) opened per pair. That gap is too small for
  // a time limit on a busy machine, so a reopen per read is shown by the open count; the time bound only catches a
  // read held up by the writer's uncommitted transaction, which would wait out the 5 s busy timeout.
  constexpr int kReads = 100;
  constexpr int64_t kBlockedBoundUs = 2'500'000; // half the 5 s busy timeout, far from any scheduling jitter
  int reads = 0;
  int64_t slowest_us = 0;
  for (int round = 0; round < kReads; ++round) {
    const auto started = std::chrono::steady_clock::now();
    auto active = calls_->ListActiveSessions();
    auto fresh = calls_->LoadSession("call:new");
    const int64_t took_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    slowest_us = std::max(slowest_us, took_us);
    ASSERT_TRUE(active) << active.error().message;
    ASSERT_EQ(active->size(), 1u);
    EXPECT_EQ(active->front().call_id, "call:before") << "the content before the commit";
    ASSERT_TRUE(fresh);
    EXPECT_FALSE(fresh->has_value());
    ++reads;
  }
  EXPECT_LT(slowest_us, kBlockedBoundUs) << "two reads took " << slowest_us << " us";
  EXPECT_EQ(reads, kReads);
  EXPECT_EQ(calls_->ReadConnectionOpensForTest(), 1u) << "every read used the one connection";

  ASSERT_EQ(writer.Exec("COMMIT;"), SQLITE_OK);
  auto active = calls_->ListActiveSessions().value();
  ASSERT_EQ(active.size(), 1u);
  EXPECT_EQ(active.front().call_id, "call:new");
}

TEST_F(CallSessionStoreTest, AReadAfterAFailedPrepareSeesLaterWrites) {
  ASSERT_NO_FATAL_FAILURE(UseWal(data_dir_ / "profile.db"));
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:x")));
  ASSERT_TRUE(calls_->UpsertParticipant(Joined("call:x", "account:alice")));
  ASSERT_EQ(calls_->ListParticipants("call:x").value().size(), 1u);

  {
    OtherConnection other(data_dir_ / "profile.db");
    ASSERT_EQ(other.Exec("DROP TABLE call_participants;"), SQLITE_OK);
  }
  // The first read finds the schema changed under it; the next one cannot even prepare (the table is gone).
  EXPECT_FALSE(calls_->ListParticipants("call:x"));
  EXPECT_FALSE(calls_->ListParticipants("call:x"));
  EXPECT_FALSE(calls_->CountJoined("call:x"));
  {
    const SqliteCallSessionStore again((data_dir_ / "profile.db").string()); // makes the table again
    ASSERT_TRUE(again.UpsertParticipant(Joined("call:x", "account:bob")));
  }
  auto parts = calls_->ListParticipants("call:x");
  ASSERT_TRUE(parts) << parts.error().message;
  ASSERT_EQ(parts->size(), 1u);
  EXPECT_EQ(parts->front().identity, "account:bob");
  EXPECT_EQ(calls_->ReadConnectionOpensForTest(), 1u) << "the same connection, not a reopened one";
}

TEST_F(CallSessionStoreTest, AReadAfterAFailedStepSeesLaterWrites) {
  ASSERT_NO_FATAL_FAILURE(UseWal(data_dir_ / "profile.db"));
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:x")));
  ASSERT_TRUE(calls_->UpsertParticipant(Joined("call:x", "account:alice")));
  calls_.reset();
  ASSERT_NO_FATAL_FAILURE(ZeroTableRootPage(data_dir_ / "profile.db", "call_sessions"));
  calls_ = std::make_unique<SqliteCallSessionStore>((data_dir_ / "profile.db").string());

  auto active = calls_->ListActiveSessions();
  ASSERT_FALSE(active);
  EXPECT_NE(active.error().message.find("rc=11"), std::string::npos) << active.error().message;
  {
    OtherConnection other(data_dir_ / "profile.db");
    ASSERT_EQ(other.Exec("INSERT INTO call_participants (call_id, identity, state) VALUES "
                         "('call:x', 'account:bob', 'joined');"),
              SQLITE_OK);
  }
  auto parts = calls_->ListParticipants("call:x");
  ASSERT_TRUE(parts) << parts.error().message;
  EXPECT_EQ(parts->size(), 2u) << "bob, written after the failed read, is there";
  EXPECT_EQ(calls_->CountJoined("call:x").value(), 2u);
}

// The call's own thread writes while the UI thread and a worker read (CallStack's store). Run with --gtest_repeat=20.
TEST_F(CallSessionStoreTest, TwoReadersAndAWriterRunTogether) {
  ASSERT_NO_FATAL_FAILURE(UseWal(data_dir_ / "profile.db"));
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:busy")));
  ASSERT_TRUE(calls_->UpsertPendingInvite(Invite("call:busy", "account:me")));
  constexpr int kTimes = 2000;
  std::mutex failures_mutex;
  std::vector<std::string> failures;
  const auto note = [&](const std::string& what) {
    std::lock_guard<std::mutex> lock(failures_mutex);
    failures.push_back(what);
  };
  const SqliteCallSessionStore* store = calls_.get();
  std::atomic<int> finished{0};
  std::vector<std::thread> threads;
  for (int reader = 0; reader < 2; ++reader) {
    threads.emplace_back([&, store]() {
      for (int i = 0; i < kTimes; ++i) {
        if (auto parts = store->ListParticipants("call:busy"); !parts) {
          note(parts.error().message);
        }
        if (auto active = store->ListActiveSessions(); !active || active->size() != 1) {
          note(active ? "active calls: " + std::to_string(active->size()) : active.error().message);
        }
        if (auto loaded = store->LoadSession("call:busy"); !loaded || !loaded->has_value()) {
          note(loaded ? "call:busy not found" : loaded.error().message);
        }
        if (auto pending = store->ListPendingInvitesForInvitee("account:me"); !pending) {
          note(pending.error().message);
        }
      }
      ++finished;
    });
  }
  threads.emplace_back([&, store]() {
    for (int i = 0; i < kTimes; ++i) {
      const std::string who = "account:p" + std::to_string(i % 10);
      if (auto put = store->UpsertParticipant(Joined("call:busy", who)); !put) {
        note("write: " + put.error().message);
      }
      if (i % 50 == 0) {
        if (auto put = store->UpsertSession(ActiveSession("call:busy")); !put) {
          note("write: " + put.error().message);
        }
      }
    }
    ++finished;
  });
  JoinWithin(threads, finished, std::chrono::seconds(120));

  for (const std::string& failure : failures) {
    ADD_FAILURE() << failure;
  }
  EXPECT_EQ(calls_->CountJoined("call:busy").value(), 10u);
  EXPECT_EQ(calls_->ReadConnectionOpensForTest(), 1u);
}

// A read in flight holds the connection: destroying the store meanwhile lets that read finish on it, and then the
// connection is closed and the files can go. Run with --gtest_repeat=50.
TEST_F(CallSessionStoreTest, DestroyingTheStoreWhileAReadIsInFlightLetsTheReadFinish) {
  ASSERT_NO_FATAL_FAILURE(UseWal(data_dir_ / "profile.db"));
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:x")));
  ASSERT_TRUE(calls_->UpsertParticipant(Joined("call:x", "account:alice")));
  constexpr int kPausedRead = 200;
  std::mutex mutex;
  std::condition_variable changed;
  bool holding = false;
  bool destroyed = false;
  std::atomic<int> reads{0};
  SqliteCallSessionStore::SetReadHookForTest([&]() {
    if (++reads != kPausedRead) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex);
    holding = true;
    changed.notify_all();
    changed.wait_for(lock, std::chrono::seconds(10), [&]() { return destroyed; });
  });
  const SqliteCallSessionStore* store = calls_.get();
  std::atomic<int> finished{0};
  std::atomic<int> failed{0};
  std::atomic<bool> paused_read_ok{false};
  std::vector<std::thread> threads;
  threads.emplace_back([&, store]() {
    // Reads back to back; the last one is paused by the hook (holding the connection) while the store goes.
    for (int i = 1; i <= kPausedRead; ++i) {
      auto parts = store->ListParticipants("call:x");
      const bool ok = parts && parts->size() == 1;
      if (i == kPausedRead) {
        paused_read_ok = ok;
      } else if (!ok) {
        ++failed;
      }
    }
    ++finished;
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(10), [&]() { return holding; }));
  }
  calls_.reset();
  {
    std::lock_guard<std::mutex> lock(mutex);
    destroyed = true;
  }
  changed.notify_all();
  JoinWithin(threads, finished, std::chrono::seconds(30));
  SqliteCallSessionStore::SetReadHookForTest(nullptr);

  EXPECT_EQ(failed.load(), 0);
  EXPECT_TRUE(paused_read_ok.load()) << "the read in flight finished on the open connection";
  for (const char* suffix : {"", "-wal", "-shm"}) {
    const std::filesystem::path file = data_dir_ / (std::string("profile.db") + suffix);
    std::error_code ec;
    std::filesystem::remove(file, ec);
    EXPECT_FALSE(ec) << file << ": " << ec.message();
    EXPECT_FALSE(std::filesystem::exists(file)) << file;
  }
}

TEST_F(CallSessionStoreTest, TheFirstReadOfAFreshProfileIsEmpty) {
  ASSERT_FALSE(std::filesystem::exists(data_dir_ / "profile.db"));
  auto active = calls_->ListActiveSessions();
  ASSERT_TRUE(active) << active.error().message;
  EXPECT_TRUE(active->empty());
  EXPECT_FALSE(calls_->LoadSession("call:x").value().has_value());
  EXPECT_TRUE(calls_->ListSessionsForGroup("group:g").value().empty());
  EXPECT_TRUE(calls_->ListRecentSessions(5).value().empty());
  EXPECT_TRUE(calls_->ListParticipants("call:x").value().empty());
  EXPECT_EQ(calls_->CountJoined("call:x").value(), 0u);
  EXPECT_FALSE(calls_->LoadPendingInvite("call:x", "account:me").value().has_value());
  EXPECT_TRUE(calls_->ListPendingInvitesForInvitee("account:me").value().empty());

  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:x")));
  ASSERT_TRUE(calls_->UpsertParticipant(Joined("call:x", "account:alice")));
  EXPECT_EQ(calls_->ListActiveSessions().value().size(), 1u);
  EXPECT_EQ(calls_->ListParticipants("call:x").value().size(), 1u);
  EXPECT_EQ(calls_->ReadConnectionOpensForTest(), 1u);
}

// After the last connection closes a WAL file has no -wal / -shm beside it; the read-only connection still opens it.
TEST_F(CallSessionStoreTest, AWalProfileWithoutItsSideFilesOpensForReading) {
  ASSERT_NO_FATAL_FAILURE(UseWal(data_dir_ / "profile.db"));
  {
    const SqliteCallSessionStore earlier((data_dir_ / "profile.db").string());
    ASSERT_TRUE(earlier.UpsertSession(ActiveSession("call:x")));
    ASSERT_EQ(earlier.ListActiveSessions().value().size(), 1u);
  }
  {
    // A read-only connection closing last cannot checkpoint, so it leaves the side files (nothing is lost: the next
    // read-write open recovers them). A read-write connection closing last folds them in and removes them.
    OtherConnection last(data_dir_ / "profile.db");
    ASSERT_EQ(last.Exec("SELECT count(*) FROM call_sessions;"), SQLITE_OK);
  }
  ASSERT_FALSE(std::filesystem::exists(data_dir_ / "profile.db-shm"));
  ASSERT_FALSE(std::filesystem::exists(data_dir_ / "profile.db-wal"));
  auto active = calls_->ListActiveSessions();
  ASSERT_TRUE(active) << active.error().message;
  EXPECT_EQ(active->size(), 1u);
}

TEST_F(CallSessionStoreTest, AProfileWhoseTablesCannotBeMadeIsAnErrorOnEveryRead) {
  // Not a database: the open succeeds, making the tables fails.
  const std::filesystem::path garbage = data_dir_ / "garbage.db";
  {
    std::ofstream file(garbage, std::ios::binary);
    file << std::string(8192, 'x');
  }
  const SqliteCallSessionStore not_a_db(garbage.string());
  const SqliteCallSessionStore no_dir((data_dir_ / "missing" / "profile.db").string());
  for (const SqliteCallSessionStore* broken : {&not_a_db, &no_dir}) {
    for (int i = 0; i < 3; ++i) {
      EXPECT_FALSE(broken->ListActiveSessions());
      EXPECT_FALSE(broken->LoadSession("call:x"));
      EXPECT_FALSE(broken->ListParticipants("call:x"));
      EXPECT_FALSE(broken->CountJoined("call:x"));
      EXPECT_FALSE(broken->ListPendingInvitesForInvitee("account:me"));
      EXPECT_FALSE(broken->UpsertSession(ActiveSession("call:x")));
    }
    EXPECT_EQ(broken->ReadConnectionOpensForTest(), 0u);
  }
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:x")));
  EXPECT_EQ(calls_->ListActiveSessions().value().size(), 1u) << "a store on a good file is unaffected";
}

// Every read goes through the read-only connection: a read method that wrote would fail there, never write.
TEST_F(CallSessionStoreTest, TheReadConnectionCannotWrite) {
  ASSERT_TRUE(calls_->ListActiveSessions());
  auto wrote = calls_->ExecOnReadConnectionForTest(
      "INSERT INTO call_sessions (call_id, state, created_at) VALUES ('call:sneaked', 'active', 1);");
  ASSERT_FALSE(wrote);
  EXPECT_NE(wrote.error().message.find("readonly"), std::string::npos) << wrote.error().message;
  EXPECT_FALSE(calls_->LoadSession("call:sneaked").value().has_value());
  EXPECT_TRUE(calls_->ListActiveSessions().value().empty());
  EXPECT_EQ(calls_->ReadConnectionOpensForTest(), 1u);
}

// A failed prepare says why (SQLite's own words), not just which read it was.
TEST_F(CallSessionStoreTest, AFailedPrepareCarriesSqlitesMessage) {
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:x")));
  ASSERT_TRUE(calls_->ListActiveSessions()); // the shared read connection is open
  {
    OtherConnection other(data_dir_ / "profile.db");
    ASSERT_EQ(other.Exec("DROP TABLE call_participants;"), SQLITE_OK);
  }
  auto listed = calls_->ListParticipants("call:x");
  ASSERT_FALSE(listed);
  EXPECT_NE(listed.error().message.find("no such table"), std::string::npos) << listed.error().message;
}

// The reset flows delete profile.db only after the hub shut the stores down: a store that is gone leaves no handle,
// and a store made afterwards reads the new file, not the deleted one.
TEST_F(CallSessionStoreTest, ADestroyedStoreLeavesProfileDbFreeToReplace) {
  const std::filesystem::path db = data_dir_ / "profile.db";
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:old")));
  ASSERT_EQ(calls_->ListActiveSessions().value().size(), 1u);
  calls_.reset();
  std::error_code ec;
  ASSERT_TRUE(std::filesystem::remove(db, ec)) << ec.message();
  calls_ = std::make_unique<SqliteCallSessionStore>(db.string());
  EXPECT_TRUE(calls_->ListActiveSessions().value().empty());
  ASSERT_TRUE(calls_->UpsertSession(ActiveSession("call:new")));
  const auto active = calls_->ListActiveSessions().value();
  ASSERT_EQ(active.size(), 1u);
  EXPECT_EQ(active[0].call_id, "call:new");
}

} // namespace
} // namespace pbr
