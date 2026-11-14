#include "lib/storage/SqliteCallSessionStore.h"
#include "lib/storage/SqliteChatDbLadders.h"
#include "lib/storage/SqliteChatStoreGate.h"
#include "lib/storage/SqliteThreadLocalPrefsStore.h"
#include "lib/storage/SqliteMigrations.h"
#include "lib/storage/SqliteThreadSchema.h"
#include "lib/storage/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

// D105 (DATA_LAYOUT § Schema versions): each chat database moves up a ladder of versions, from a floor; the step
// into today's schema keeps a tester's chats whatever columns the per-open ALTERs of before had added.

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x30 + i);
  }
  return dek;
}

/** The columns the per-open ALTERs added to profile.db before the ladder (table, column). */
const std::vector<std::pair<std::string, std::string>>& ProfileAlterColumns() {
  static const std::vector<std::pair<std::string, std::string>> columns = {
      {"threads", "listed"},
      {"chat_targets", "auto_key_session_id"},
      {"chat_targets", "auto_key_init_pending"},
      {"chat_targets", "auto_key_peer_heard_ms"},
      {"chat_targets", "restart_pending"},
      {"pending_group_invites", "group_title"},
      {"pending_group_invites", "roster_epoch"},
      {"pending_group_invites", "outbound"},
      {"pending_group_invites", "owner_identity"},
      {"group_metadata", "title_version"},
      {"group_metadata", "roster_statement_json"},
      {"group_metadata", "sync_state"},
      {"group_metadata", "sync_request_id"},
      {"group_metadata", "sync_requested_at"},
      {"group_rosters", "added_by"},
      {"thread_local_prefs", "mute_mentions"},
      {"thread_local_prefs", "share_ai_confirmed"},
      {"call_sessions", "video_allowed"},
      {"call_sessions", "planned_hop"},
      {"call_sessions", "planned_hop_ma"},
      {"call_sessions", "initiator_identity"},
      {"pending_call_invites", "video_allowed"},
      {"call_participants", "on_hold"},
      {"call_participants", "video_paused"},
      {"call_participants", "brought_by"},
  };
  return columns;
}

class SqliteMigrationsTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_browser_sqlite_migrations_" +
                 std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
  }

  void TearDown() override {
    store_.reset();
    CloseDb();
    std::filesystem::remove_all(data_dir_);
  }

  std::string ProfileDb() const { return SqliteThreadProfileDbFile(data_dir_.string()); }

  /** A store on the profile, unlocked. */
  SqliteThreadStore& OpenStore() {
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    EXPECT_TRUE(store_->SetDek(TestDek()));
    return *store_;
  }

  void CloseStore() {
    if (store_) {
      store_->Flush();
      store_.reset();
    }
  }

  /** A profile whose catalog holds one AI chat with one message, written at today's schema. */
  void WriteProfileWithAChat() {
    SqliteThreadStore& store = OpenStore();
    Thread thread;
    thread.id = "thread-kept";
    thread.kind = ThreadKind::Ai;
    thread.title = "Kept";
    thread.updated_at = 7;
    ASSERT_TRUE(store.UpsertThread(thread));
    ThreadMessage message;
    message.id = "msg-kept";
    message.thread_id = thread.id;
    message.sender_contact_id = kLocalSelfContactId;
    message.text = "still here";
    message.timestamp = 7;
    ASSERT_TRUE(store.AppendMessage(message));
    CloseStore();
  }

  sqlite3* Db(const std::string& path) {
    CloseDb();
    EXPECT_EQ(sqlite3_open(path.c_str(), &db_), SQLITE_OK);
    return db_;
  }
  void CloseDb() {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }
  void Exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK) << sql << ": " << (err ? err : "");
    sqlite3_free(err);
  }
  int Version(const std::string& path) {
    auto version = SqliteUserVersion(Db(path));
    CloseDb();
    return version ? *version : -1;
  }
  bool HasColumn(const std::string& path, const std::string& table, const std::string& column) {
    auto has = SqliteColumnExists(Db(path), table, column);
    CloseDb();
    return has && *has;
  }

  /** The tester's profile.db: today's tables at schema 4, as the stores left it before the ALTERs ran on it. */
  void MakeTesterProfileDbWithoutTheAlteredColumns() {
    sqlite3* db = Db(ProfileDb());
    for (const auto& [table, column] : ProfileAlterColumns()) {
      Exec(db, "ALTER TABLE " + table + " DROP COLUMN " + column + ";");
    }
    Exec(db, "ALTER TABLE call_sessions ADD COLUMN session_kind TEXT NOT NULL DEFAULT 'group';");
    Exec(db, "ALTER TABLE pending_call_invites ADD COLUMN session_kind TEXT NOT NULL DEFAULT 'group';");
    Exec(db, "PRAGMA user_version = 4;");
    CloseDb();
  }

  void ExpectTheChatIsKept() {
    SqliteThreadStore& store = OpenStore();
    auto threads = store.ListThreads();
    ASSERT_TRUE(threads) << threads.error().message;
    ASSERT_EQ(threads->size(), 1u);
    EXPECT_EQ(threads->front().id, "thread-kept");
    auto page = store.GetMessagesPage("thread-kept", std::nullopt, 10);
    ASSERT_TRUE(page) << page.error().message;
    ASSERT_EQ(page->size(), 1u);
    EXPECT_EQ(page->front().text, "still here");
    CloseStore();
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  sqlite3* db_ = nullptr;
};

TEST_F(SqliteMigrationsTest, AFreshInstallGetsTheCurrentSchema) {
  SqliteThreadStore& store = OpenStore();
  ASSERT_TRUE(store.ListThreads());
  CloseStore();
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion);
  for (const auto& [table, column] : ProfileAlterColumns()) {
    EXPECT_TRUE(HasColumn(ProfileDb(), table, column)) << table << "." << column << " is in the created schema";
  }
  EXPECT_FALSE(HasColumn(ProfileDb(), "call_sessions", "session_kind"));
  EXPECT_FALSE(TakeChatStoreResetNotice(data_dir_.string())) << "nothing was reset";
}

TEST_F(SqliteMigrationsTest, ATesterDatabaseWithoutTheAlteredColumnsMigratesAndKeepsItsRows) {
  WriteProfileWithAChat();
  MakeTesterProfileDbWithoutTheAlteredColumns();
  ASSERT_FALSE(HasColumn(ProfileDb(), "threads", "listed"));

  ExpectTheChatIsKept();

  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion);
  for (const auto& [table, column] : ProfileAlterColumns()) {
    EXPECT_TRUE(HasColumn(ProfileDb(), table, column)) << table << "." << column;
  }
  EXPECT_FALSE(HasColumn(ProfileDb(), "call_sessions", "session_kind")) << "the legacy row kind is dropped";
  EXPECT_FALSE(HasColumn(ProfileDb(), "pending_call_invites", "session_kind"));
  EXPECT_FALSE(std::filesystem::exists(ProfileDb() + ".pre-migration")) << "the backup copy is gone";
  // Its calls store reads and writes its rows (the columns it names are there).
  SqliteCallSessionStore calls(ProfileDb());
  EXPECT_TRUE(calls.ListRecentSessions(5)) << "call rows read at the baseline";
}

TEST_F(SqliteMigrationsTest, ATesterDatabaseThatAlreadyHasTheColumnsMigratesToo) {
  WriteProfileWithAChat();
  Exec(Db(ProfileDb()), "PRAGMA user_version = 4;");
  CloseDb();

  ExpectTheChatIsKept();
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion);
}

TEST_F(SqliteMigrationsTest, RunningTheLadderTwiceIsANoOp) {
  WriteProfileWithAChat();
  Exec(Db(ProfileDb()), "PRAGMA user_version = 4;");
  CloseDb();
  sqlite3* db = Db(ProfileDb());
  auto first = RunSqliteLadder(db, ProfileDb(), ProfileDbLadder());
  ASSERT_TRUE(first) << first.error().message;
  EXPECT_EQ(*first, SqliteLadderOutcome::Migrated);
  auto second = RunSqliteLadder(db, ProfileDb(), ProfileDbLadder());
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_EQ(*second, SqliteLadderOutcome::Current);
  CloseDb();
  ExpectTheChatIsKept();
}

TEST_F(SqliteMigrationsTest, BelowTheFloorTheChatStoreIsResetAndSaysSoOnce) {
  WriteProfileWithAChat();
  Exec(Db(ProfileDb()), "PRAGMA user_version = " + std::to_string(kSqliteThreadProfileFloorVersion - 1) + ";");
  CloseDb();

  SqliteThreadStore& store = OpenStore();
  auto threads = store.ListThreads();
  ASSERT_TRUE(threads) << threads.error().message;
  EXPECT_TRUE(threads->empty()) << "too old to upgrade: the chats were reset";
  CloseStore();
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion);
  EXPECT_FALSE(std::filesystem::exists(SqliteThreadDbFile(data_dir_.string(), "thread-kept")));
  EXPECT_TRUE(TakeChatStoreResetNotice(data_dir_.string()));
  EXPECT_FALSE(TakeChatStoreResetNotice(data_dir_.string())) << "once";
}

TEST_F(SqliteMigrationsTest, ANewerDatabaseIsRefusedAndLeftAlone) {
  WriteProfileWithAChat();
  Exec(Db(ProfileDb()), "PRAGMA user_version = " + std::to_string(kSqliteThreadProfileUserVersion + 1) + ";");
  CloseDb();

  SqliteThreadStore& store = OpenStore();
  auto threads = store.ListThreads();
  ASSERT_FALSE(threads);
  EXPECT_NE(threads.error().message.find("Incompatible profile.db"), std::string::npos) << threads.error().message;
  CloseStore();
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion + 1);
  EXPECT_TRUE(std::filesystem::exists(SqliteThreadDbFile(data_dir_.string(), "thread-kept"))) << "nothing reset";
}

TEST_F(SqliteMigrationsTest, AThreadDbWithoutSenderDeviceMigratesAndKeepsItsMessages) {
  WriteProfileWithAChat();
  const std::string thread_db = SqliteThreadDbFile(data_dir_.string(), "thread-kept");
  sqlite3* db = Db(thread_db);
  Exec(db, "ALTER TABLE messages DROP COLUMN sender_device;");
  Exec(db, "PRAGMA user_version = 2;");
  CloseDb();

  ExpectTheChatIsKept();
  EXPECT_EQ(Version(thread_db), kSqliteThreadUserVersion);
  EXPECT_TRUE(HasColumn(thread_db, "messages", "sender_device"));
}

TEST_F(SqliteMigrationsTest, AThreadDbBelowItsFloorLosesItsMessagesNotItsChat) {
  WriteProfileWithAChat();
  const std::string thread_db = SqliteThreadDbFile(data_dir_.string(), "thread-kept");
  Exec(Db(thread_db), "PRAGMA user_version = " + std::to_string(kSqliteThreadFloorVersion - 1) + ";");
  CloseDb();

  SqliteThreadStore& store = OpenStore();
  auto page = store.GetMessagesPage("thread-kept", std::nullopt, 10);
  ASSERT_TRUE(page) << page.error().message;
  EXPECT_TRUE(page->empty());
  auto threads = store.ListThreads();
  ASSERT_TRUE(threads);
  EXPECT_EQ(threads->size(), 1u) << "the chat stays in the catalog";
  CloseStore();
  EXPECT_EQ(Version(thread_db), kSqliteThreadUserVersion);
  EXPECT_TRUE(TakeChatStoreResetNotice(data_dir_.string()));
}

// profile.db 5 → 6 (multi-relay MR001): nothing in profile.db maps a retired `relay:` id to an Account ID, so what is
// keyed by one is dropped — a chat's session keys, roster rows, invites, owed sends, call rows, applied lane ids — and
// a chat whose peer was one keeps its messages and loses its peer; everything keyed by an Account ID stays.
TEST_F(SqliteMigrationsTest, AProfileDbAt5DropsWhatARelayIdKeys) {
  WriteProfileWithAChat();
  sqlite3* db = Db(ProfileDb());
  Exec(db,
       "INSERT INTO threads (id, kind, channel, peer_identity_kind, peer_identity_value, title, "
       "participant_contact_ids, updated_at) VALUES ('thread-relay', 'direct', 'e2e_public', 'relay_user', "
       "'relay:old', 'Old', '[]', 1), ('thread-account', 'direct', 'e2e_public', 'account', 'account:ann', "
       "'Ann', '[]', 2);");
  Exec(db,
       "INSERT INTO chat_targets (peer_identity_kind, peer_identity_value, channel, local_thread_id) VALUES "
       "('relay_user', 'relay:old', 'e2e_public', 'thread-relay'), "
       "('account', 'account:ann', 'e2e_public', 'thread-account');");
  Exec(db, "INSERT INTO group_rosters (group_id, member_identity) VALUES ('g', 'relay:old'), ('g', 'account:ann');");
  Exec(db, "INSERT INTO call_participants (call_id, identity) VALUES ('c', 'relay:old'), ('c', 'account:ann');");
  Exec(db, "PRAGMA user_version = 5;");
  CloseDb();

  {
    auto threads = OpenStore().ListThreads(); // opens and migrates
    ASSERT_TRUE(threads) << threads.error().message;
    EXPECT_EQ(threads->size(), 3u) << "no chat is dropped";
    CloseStore();
  }
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion);
  db = Db(ProfileDb());
  const auto count = [&](const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK) << sql;
    const int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return n;
  };
  EXPECT_EQ(count("SELECT COUNT(*) FROM chat_targets WHERE peer_identity_value LIKE 'relay:%'"), 0);
  EXPECT_EQ(count("SELECT COUNT(*) FROM chat_targets WHERE peer_identity_value = 'account:ann'"), 1);
  EXPECT_EQ(count("SELECT COUNT(*) FROM threads WHERE id = 'thread-relay' AND peer_identity_value = ''"), 1)
      << "the chat stays, without its peer";
  EXPECT_EQ(count("SELECT COUNT(*) FROM threads WHERE peer_identity_value = 'account:ann'"), 1);
  EXPECT_EQ(count("SELECT COUNT(*) FROM group_rosters"), 1);
  EXPECT_EQ(count("SELECT COUNT(*) FROM call_participants"), 1);
  CloseDb();
}

// profile.db 6 → 7 (account card version 3): our own kept version 2 card takes the version 3 shape (OwnCards then
// re-issues it with its device list); anyone else's goes, their keys kept as a key card.
TEST_F(SqliteMigrationsTest, AProfileDbAt6StepsKeptCardsToVersion3) {
  WriteProfileWithAChat();
  sqlite3* db = Db(ProfileDb());
  Exec(db,
       "CREATE TABLE IF NOT EXISTS peer_cards (account_id TEXT PRIMARY KEY, record_json TEXT NOT NULL, "
       "updated_at INTEGER NOT NULL);");
  const std::string card =
      R"({"card_version":2,"account_id":"account:x","devices":[{"peer_id":"QmA","signing_public_key_b64":"s",)"
      R"("kem_public_key_b64":"k"}],"issued_at":5,"card_signature_b64":"sig"})";
  const auto record = [&card](const std::string& account, const std::string& source) {
    return R"({"account_id":")" + account + R"(","signing_public_key_b64":"s","kem_public_key_b64":"k",)" +
           R"("kem_binding_signature_b64":"b","issued_at":5,"devices":["QmA"],"source":")" + source +
           R"(","newest_card":)" + card + "}";
  };
  Exec(db, "INSERT INTO peer_cards VALUES ('account:me', '" + record("account:me", "self") +
               "', 1), ('account:sib', '" + record("account:sib", "sibling") + "', 1), ('account:ann', '" +
               record("account:ann", "envelope") + "', 1), ('account:bad', 'not json', 1);");
  Exec(db, "PRAGMA user_version = 6;");
  CloseDb();

  ASSERT_TRUE(OpenStore().ListThreads()); // opens and migrates
  CloseStore();
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion);
  db = Db(ProfileDb());
  const auto text = [&](const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK) << sql;
    std::string out = "<none>";
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      out = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return out;
  };
  const auto field = [&](const std::string& account, const std::string& path) {
    return text("SELECT json_extract(record_json, '" + path + "') FROM peer_cards WHERE account_id = '" + account +
                "'");
  };
  for (const std::string own : {"account:me", "account:sib"}) {
    EXPECT_EQ(field(own, "$.newest_card.card_version"), "3") << own;
    EXPECT_EQ(field(own, "$.newest_card.relays"), "[]");
    EXPECT_EQ(field(own, "$.newest_card.initiation_floor"), "0");
    EXPECT_EQ(field(own, "$.newest_card.devices[0].peer_id"), "QmA") << "the device list is kept";
    EXPECT_EQ(field(own, "$.newest_card.card_signature_b64"), "sig");
  }
  EXPECT_EQ(field("account:ann", "$.newest_card"), "<none>") << "another account's card goes";
  EXPECT_EQ(field("account:ann", "$.kem_binding_signature_b64"), "b") << "its keys stay";
  EXPECT_EQ(text("SELECT record_json FROM peer_cards WHERE account_id = 'account:bad'"), "not json");
  CloseDb();
}

// thread.db 3 → 4 (multi-relay MR001): in a 1:1 chat, a message stored with a `relay:` sender is the peer's — it takes
// the peer's Account ID; a history watermark keyed by a relay id is dropped.
TEST_F(SqliteMigrationsTest, AThreadDbAt3NamesItsPeerByAccountId) {
  SqliteThreadStore& store = OpenStore();
  Thread thread;
  thread.id = "thread-ann";
  thread.kind = ThreadKind::Direct;
  thread.channel = ThreadChannel::E2ePublic;
  thread.peer_identity_kind = "account";
  thread.peer_identity_value = "account:ann";
  thread.title = "Ann";
  thread.updated_at = 7;
  ASSERT_TRUE(store.UpsertThread(thread));
  ThreadMessage message;
  message.id = "msg-from-ann";
  message.thread_id = thread.id;
  message.sender_contact_id = "relay:old";
  message.text = "hi";
  message.timestamp = 7;
  ASSERT_TRUE(store.AppendMessage(message));
  CloseStore();
  const std::string thread_db = SqliteThreadDbFile(data_dir_.string(), "thread-ann");
  sqlite3* db = Db(thread_db);
  Exec(db,
       "INSERT INTO sync_state (peer_identity_kind, peer_identity_value, session_epoch, state_json) VALUES "
       "('relay_user', 'relay:old', 1, '{}');");
  Exec(db, "PRAGMA user_version = 3;");
  CloseDb();

  SqliteThreadStore& reopened = OpenStore();
  auto page = reopened.GetMessagesPage("thread-ann", std::nullopt, 10);
  ASSERT_TRUE(page) << page.error().message;
  ASSERT_EQ(page->size(), 1u);
  EXPECT_EQ(page->front().sender_contact_id, "account:ann");
  CloseStore();
  EXPECT_EQ(Version(thread_db), kSqliteThreadUserVersion);
  db = Db(thread_db);
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sync_state", -1, &stmt, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(stmt, 0), 0);
  sqlite3_finalize(stmt);
  CloseDb();
}

// In a chat with no 1:1 peer (a group's, here the AI's) who a `relay:` sender was cannot be told: the message is dropped.
TEST_F(SqliteMigrationsTest, AThreadDbAt3WithoutAPeerDropsMessagesFromARelayId) {
  WriteProfileWithAChat();
  const std::string thread_db = SqliteThreadDbFile(data_dir_.string(), "thread-kept");
  sqlite3* db = Db(thread_db);
  Exec(db, "UPDATE messages SET id = 'msg-relay', sender_contact_id = 'relay:old';");
  Exec(db, "PRAGMA user_version = 3;");
  CloseDb();
  SqliteThreadStore& store = OpenStore();
  auto page = store.GetMessagesPage("thread-kept", std::nullopt, 10);
  ASSERT_TRUE(page) << page.error().message;
  EXPECT_TRUE(page->empty()) << "not a 1:1 chat: no peer to name it by";
  CloseStore();
}

// Nothing opens, creates or migrates the chat store before the PIN is entered: the thread store holds profile.db
// shut until it gets the data key, and the stores sharing the file are refused meanwhile.
TEST_F(SqliteMigrationsTest, NothingIsOpenedOrMigratedBeforeUnlock) {
  WriteProfileWithAChat();
  Exec(Db(ProfileDb()), "PRAGMA user_version = 4;");
  CloseDb();

  store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
  auto locked = store_->ListThreads();
  ASSERT_FALSE(locked);
  SqliteThreadLocalPrefsStore prefs(ProfileDb());
  EXPECT_FALSE(prefs.List()) << "a store on the same file waits for the unlock too";
  EXPECT_EQ(Version(ProfileDb()), 4) << "not migrated before unlock";

  ASSERT_TRUE(store_->SetDek(TestDek())) << "the unlock opens the store";
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion) << "migrated at unlock";
  EXPECT_TRUE(prefs.List()) << "open once the chat store is";
  CloseStore();
  ExpectTheChatIsKept();
}

TEST_F(SqliteMigrationsTest, AFreshProfileCreatesNothingBeforeUnlock) {
  store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
  EXPECT_FALSE(store_->ListThreads());
  EXPECT_FALSE(std::filesystem::exists(ProfileDb())) << "no file before unlock";
  ASSERT_TRUE(store_->SetDek(TestDek()));
  EXPECT_TRUE(std::filesystem::exists(ProfileDb()));
  EXPECT_TRUE(store_->ListThreads());
}

// A chat store newer than this build does not fail the unlock: it stays shut and says why, so the app can offer a
// recovery; a reset (the chat store removed, a new store opened) continues as on a fresh profile.
TEST_F(SqliteMigrationsTest, ANewerStoreStaysShutAndSaysWhyThenAResetOpensAFreshOne) {
  WriteProfileWithAChat();
  Exec(Db(ProfileDb()), "PRAGMA user_version = " + std::to_string(kSqliteThreadProfileUserVersion + 1) + ";");
  CloseDb();

  store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
  ASSERT_TRUE(store_->SetDek(TestDek())) << "the unlock itself goes through";
  auto failure = chat_store_gate::OpenFailure(ProfileDb());
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->problem, ChatStoreProblem::Newer);
  EXPECT_FALSE(store_->ListThreads());
  SqliteThreadLocalPrefsStore prefs(ProfileDb());
  EXPECT_FALSE(prefs.List()) << "the stores sharing the file are refused too";

  // Reset chats: the hub's stores go, the chat store is removed, new stores open at the unlock.
  store_.reset();
  std::filesystem::remove_all(SqliteThreadsRoot(data_dir_.string()));
  SqliteThreadStore& fresh = OpenStore();
  EXPECT_FALSE(chat_store_gate::OpenFailure(ProfileDb()).has_value());
  auto threads = fresh.ListThreads();
  ASSERT_TRUE(threads) << threads.error().message;
  EXPECT_TRUE(threads->empty());
  EXPECT_TRUE(prefs.List());
}

TEST_F(SqliteMigrationsTest, AFailedUpgradeStaysShutAsNotUpgradedAtItsOldVersion) {
  WriteProfileWithAChat();
  sqlite3* db = Db(ProfileDb());
  // A tester's file the baseline cannot finish: session_kind is indexed, so dropping it fails.
  Exec(db, "ALTER TABLE call_sessions ADD COLUMN session_kind TEXT NOT NULL DEFAULT 'group';");
  Exec(db, "CREATE INDEX idx_kind ON call_sessions(session_kind); PRAGMA user_version = 4;");
  CloseDb();

  SqliteThreadStore& store = OpenStore();
  auto failure = chat_store_gate::OpenFailure(ProfileDb());
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->problem, ChatStoreProblem::NotUpgraded) << failure->message;
  EXPECT_FALSE(store.ListThreads());
  CloseStore();
  EXPECT_EQ(Version(ProfileDb()), 4) << "rolled back to the version it had";
  EXPECT_TRUE(std::filesystem::exists(SqliteThreadDbFile(data_dir_.string(), "thread-kept"))) << "nothing reset";
}

// The runner itself, on a ladder of its own: a step that fails leaves the database as it was — the steps before it
// undone too, at its old version — and the backup copy gone.
TEST_F(SqliteMigrationsTest, AFailedStepRestoresTheBackupAtTheOldVersion) {
  const std::string path = (data_dir_ / "ladder.db").string();
  sqlite3* db = Db(path);
  Exec(db, "PRAGMA journal_mode=WAL;");
  Exec(db, "CREATE TABLE kept (v TEXT); INSERT INTO kept VALUES ('row'); PRAGMA user_version = 1;");
  int ran = 0;
  const ByteVector* seen_dek = nullptr;
  const ByteVector dek = TestDek();
  SqliteSchemaLadder ladder{
      "ladder.db",
      1,
      3,
      [](const SqliteMigrationContext&) { return Roe<void>(); },
      {
          {1,
           [&ran, &seen_dek](const SqliteMigrationContext& c) -> Roe<void> {
             ++ran;
             seen_dek = c.dek;
             return sqlite3_exec(c.db, "CREATE TABLE added (v TEXT); DELETE FROM kept;", nullptr, nullptr, nullptr) ==
                            SQLITE_OK
                        ? Roe<void>()
                        : Roe<void>(Error("step 1"));
           }},
          {2, [](const SqliteMigrationContext&) -> Roe<void> { return Error("step 2 fails"); }},
      },
  };
  auto outcome = RunSqliteLadder(db, path, ladder, &dek);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(seen_dek, &dek) << "a step gets the profile's data key (it runs after unlock)";
  EXPECT_NE(outcome.error().message.find("left at schema 1"), std::string::npos) << outcome.error().message;
  EXPECT_EQ(ran, 1);
  auto version = SqliteUserVersion(db);
  ASSERT_TRUE(version);
  EXPECT_EQ(*version, 1);
  EXPECT_FALSE(*SqliteTableExists(db, "added")) << "the first step's change is undone with it";
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM kept;", -1, &stmt, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(stmt, 0), 1) << "the row the first step deleted is back";
  sqlite3_finalize(stmt);
  EXPECT_FALSE(std::filesystem::exists(path + ".pre-migration"));

  // The same ladder with its second step fixed runs both once, and again nothing.
  ladder.steps[1].apply = [](const SqliteMigrationContext&) { return Roe<void>(); };
  auto fixed = RunSqliteLadder(db, path, ladder);
  ASSERT_TRUE(fixed) << fixed.error().message;
  EXPECT_EQ(*fixed, SqliteLadderOutcome::Migrated);
  auto again = RunSqliteLadder(db, path, ladder);
  ASSERT_TRUE(again);
  EXPECT_EQ(*again, SqliteLadderOutcome::Current);
  EXPECT_EQ(ran, 2);
  CloseDb();
}

TEST_F(SqliteMigrationsTest, ALadderWithAMissingStepIsRefused) {
  sqlite3* db = Db((data_dir_ / "gap.db").string());
  SqliteSchemaLadder ladder{"gap.db",
                            1,
                            3,
                            [](const SqliteMigrationContext&) { return Roe<void>(); },
                            {{1, [](const SqliteMigrationContext&) { return Roe<void>(); }}}};
  EXPECT_FALSE(RunSqliteLadder(db, (data_dir_ / "gap.db").string(), ladder));
  CloseDb();
}

// Which ladder outcomes leave the chat store needing a recovery (the app's start-up screen): only Newer and a failed
// migration; created, current, migrated and a reset below the floor open it.
TEST(ChatStoreProblemTest, OnlyANewerStoreOrAFailedUpgradeNeedsARecovery) {
  for (const SqliteLadderOutcome opened : {SqliteLadderOutcome::Created, SqliteLadderOutcome::Current,
                                           SqliteLadderOutcome::Migrated, SqliteLadderOutcome::BelowFloor}) {
    EXPECT_FALSE(ChatStoreProblemFor(opened).has_value()) << static_cast<int>(opened);
  }
  EXPECT_EQ(ChatStoreProblemFor(SqliteLadderOutcome::Newer), ChatStoreProblem::Newer);
  EXPECT_EQ(ChatStoreProblemFor(Roe<SqliteLadderOutcome>(Error("step failed"))), ChatStoreProblem::NotUpgraded);
}

} // namespace
} // namespace pbr
