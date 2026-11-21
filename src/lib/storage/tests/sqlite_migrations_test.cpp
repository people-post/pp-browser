#include "lib/people/IdentityStore.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "lib/storage/SqliteCallSessionStore.h"
#include "lib/storage/SqliteChatDbLadders.h"
#include "lib/storage/SqliteChatStoreGate.h"
#include "lib/storage/SqliteThreadLocalPrefsStore.h"
#include "lib/storage/SqliteMigrations.h"
#include "lib/storage/SqlitePeerSessionStore.h"
#include "lib/storage/SqliteThreadSchema.h"
#include "lib/storage/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

// D105 (DATA_LAYOUT § Schema versions): each chat database moves up a ladder of versions, from a floor. Release 1
// starts clean (release-compatibility RC017): each floor is release 1's schema, so a file below it is reset and one
// above it refused; the runner's steps are tested on a ladder of their own.

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x30 + i);
  }
  return dek;
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
    sessions_.reset();
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

  /** The profile's session store, unlocked (the chat store must be open: OpenStore). */
  SqlitePeerSessionStore& Sessions() {
    sessions_ = std::make_unique<SqlitePeerSessionStore>(ProfileDb(), "profile");
    EXPECT_TRUE(sessions_->SetDek(TestDek()));
    return *sessions_;
  }

  static PeerSessionRecord Session(const std::string& account, const CryptoChannel channel, const uint32_t epoch,
                                   const std::string& device = {}) {
    PeerSessionRecord record;
    record.key = PeerSessionKey{
        .peer_identity_kind = "account", .peer_identity_value = account, .channel = channel, .peer_device = device};
    record.session_epoch = epoch;
    record.master_psk_b64 = kTestPsk;
    return record;
  }

  /**
   * A chat and the sessions a reset tells apart, at today's schema: public ones with a key (an account's and a
   * device pair's), and what a reset drops — a private session, a public one with only a retired key.
   */
  void WriteProfileWithSessions() {
    WriteProfileWithAChat();
    OpenStore();
    SqlitePeerSessionStore& sessions = Sessions();
    ASSERT_TRUE(sessions.Save(Session("account:ann", CryptoChannel::E2ePublic, 7)));
    ASSERT_TRUE(sessions.Save(Session("account:ann", CryptoChannel::E2ePublic, 3, "QmAnnPhone")));
    ASSERT_TRUE(sessions.Save(Session("account:bob", CryptoChannel::E2e, 4)));
    PeerSessionRecord retired_only = Session("account:cat", CryptoChannel::E2ePublic, 2);
    retired_only.master_psk_b64.reset();
    retired_only.retired_psks = {RetiredPskEntry{.epoch = 1, .master_psk_b64 = kTestPsk, .retired_at = 1}};
    ASSERT_TRUE(sessions.Save(retired_only));
    ASSERT_EQ(sessions.List().value().size(), 4u);
    sessions_.reset();
    CloseStore();
  }

  void SetProfileBelowTheFloor() {
    Exec(Db(ProfileDb()), "PRAGMA user_version = " + std::to_string(kSqliteThreadProfileFloorVersion - 1) + ";");
    CloseDb();
  }

  /** After the reset: the chats are gone, and the sessions left are `expected` (key → epoch), each to restart. */
  void ExpectResetKeeping(const std::vector<std::pair<PeerSessionKey, uint32_t>>& expected) {
    SqliteThreadStore& store = OpenStore();
    ASSERT_TRUE(store.ListThreads().value().empty()) << "the chats were reset";
    EXPECT_TRUE(TakeChatStoreResetNotice(data_dir_.string()));
    auto listed = Sessions().List();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed->size(), expected.size());
    for (const auto& [key, epoch] : expected) {
      auto loaded = sessions_->Load(key);
      ASSERT_TRUE(loaded) << loaded.error().message;
      ASSERT_TRUE(*loaded) << key.peer_identity_value << " " << key.peer_device << " is kept";
      EXPECT_EQ((*loaded)->session_epoch, epoch) << "its epoch is kept";
      EXPECT_EQ((*loaded)->master_psk_b64, std::optional<std::string>(kTestPsk)) << "its key reads under the DEK";
      EXPECT_TRUE((*loaded)->restart_pending) << "the pair starts its next epoch";
    }
    sessions_.reset();
    CloseStore();
    EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion);
  }

  static constexpr const char* kTestPsk = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<SqlitePeerSessionStore> sessions_;
  sqlite3* db_ = nullptr;
};

TEST_F(SqliteMigrationsTest, AFreshInstallGetsTheCurrentSchema) {
  SqliteThreadStore& store = OpenStore();
  ASSERT_TRUE(store.ListThreads());
  CloseStore();
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion);
  EXPECT_FALSE(TakeChatStoreResetNotice(data_dir_.string())) << "nothing was reset";
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

// Our own account card lives beside the identity, outside the chat store: a store too old to upgrade goes, it stays
// (so the next start's card is the account's list, not a fresh one that signs out the other devices).
TEST_F(SqliteMigrationsTest, BelowTheFloorTheResetLeavesOurCardBesideTheIdentity) {
  {
    IdentityStore identity(data_dir_.string(), "profile");
    ASSERT_TRUE(identity.SetDek(TestDek()));
    const LocalIdentity me = identity.LoadOrCreate().value();
    AccountCard card;
    card.keys = KeyCard{
        .account_id = me.account_id,
        .signing_public_key_b64 = me.account_signing_public_key_b64,
        .kem_public_key_b64 = me.kem_public_key_b64,
        .kem_binding_signature_b64 =
            identity.SignBytes(KemBindingSignBytes(me.account_signing_public_key_b64, me.kem_public_key_b64)).value()};
    card.devices = {AccountCardDevice{.peer_id = me.peer_id,
                                      .signing_public_key_b64 = me.public_key_b64,
                                      .kem_public_key_b64 = me.device_kem_public_key_b64}};
    card.issued_at = 1'761'235'200'000;
    ASSERT_TRUE(identity.KeepOwnCard(
        SignAccountCard(card, [&identity](const ByteVector& bytes) { return identity.SignBytes(bytes); }).value()));
  }
  WriteProfileWithAChat();
  Exec(Db(ProfileDb()), "PRAGMA user_version = " + std::to_string(kSqliteThreadProfileFloorVersion - 1) + ";");
  CloseDb();

  SqliteThreadStore& store = OpenStore();
  ASSERT_TRUE(store.ListThreads().value().empty()) << "the chats were reset";
  CloseStore();
  IdentityStore identity(data_dir_.string(), "profile");
  ASSERT_TRUE(identity.SetDek(TestDek()));
  auto kept = identity.OwnCard();
  ASSERT_TRUE(kept) << kept.error().message;
  ASSERT_TRUE(*kept) << "our card outlived the reset";
  EXPECT_EQ((*kept)->issued_at, 1'761'235'200'000);
}

// A reset below the floor keeps what Me → Storage → Reset chats keeps: the public sessions with a key, each pair to
// restart in its next epoch — else this device numbers from 1 again in an epoch the peer holds messages of, and the
// peer drops them (D014, #378). Read from the old file as it is.
TEST_F(SqliteMigrationsTest, BelowTheFloorTheResetKeepsThePublicSessions) {
  WriteProfileWithSessions();
  SetProfileBelowTheFloor();

  const PeerSessionKey account{"account", "account:ann", CryptoChannel::E2ePublic, ""};
  const PeerSessionKey device{"account", "account:ann", CryptoChannel::E2ePublic, "QmAnnPhone"};
  ExpectResetKeeping({{account, 7}, {device, 3}});
  OpenStore();
  auto seq = Sessions().AllocateSenderSeq(device);
  ASSERT_TRUE(seq) << seq.error().message;
  EXPECT_EQ(*seq, 1u) << "the device pair's numbering starts over (in the next epoch, restart_pending)";
}

// A file from before some of today's session columns: what it has is kept, the rest reads as the store's default.
TEST_F(SqliteMigrationsTest, BelowTheFloorAnOlderSessionTableStillKeepsItsPublicSessions) {
  WriteProfileWithSessions();
  sqlite3* db = Db(ProfileDb());
  for (const char* column : {"auto_key_session_id", "auto_key_init_pending", "auto_key_peer_heard_ms",
                             "restart_pending", "key_scope", "thread_kem_pk_b64", "psk_fingerprint"}) {
    Exec(db, std::string("ALTER TABLE chat_targets DROP COLUMN ") + column + ";");
  }
  Exec(db, "DROP TABLE device_sessions;");
  CloseDb();
  SetProfileBelowTheFloor();

  ExpectResetKeeping({{PeerSessionKey{"account", "account:ann", CryptoChannel::E2ePublic, ""}, 7}});
}

TEST_F(SqliteMigrationsTest, BelowTheFloorAFileWithoutSessionTablesResetsCleanly) {
  WriteProfileWithSessions();
  sqlite3* db = Db(ProfileDb());
  Exec(db, "DROP TABLE chat_targets;");
  Exec(db, "DROP TABLE device_sessions;");
  CloseDb();
  SetProfileBelowTheFloor();

  ExpectResetKeeping({});
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

// Nothing opens, creates, migrates or resets the chat store before the PIN is entered: the thread store holds
// profile.db shut until it gets the data key, and the stores sharing the file are refused meanwhile.
TEST_F(SqliteMigrationsTest, NothingIsOpenedOrResetBeforeUnlock) {
  WriteProfileWithAChat();
  SetProfileBelowTheFloor();

  store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
  auto locked = store_->ListThreads();
  ASSERT_FALSE(locked);
  SqliteThreadLocalPrefsStore prefs(ProfileDb());
  EXPECT_FALSE(prefs.List()) << "a store on the same file waits for the unlock too";
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileFloorVersion - 1) << "not touched before unlock";
  EXPECT_TRUE(std::filesystem::exists(SqliteThreadDbFile(data_dir_.string(), "thread-kept"))) << "nothing reset";

  ASSERT_TRUE(store_->SetDek(TestDek())) << "the unlock opens the store";
  EXPECT_EQ(Version(ProfileDb()), kSqliteThreadProfileUserVersion) << "reset at unlock";
  EXPECT_TRUE(prefs.List()) << "open once the chat store is";
  EXPECT_TRUE(store_->ListThreads().value().empty());
  CloseStore();
}

// Release 1 is every store's start (RC017): the floors are its schemas, profile.db 9 and thread.db 4, and no step
// from before it is kept. A later change adds a step above the floor; only a milestone moves the floor.
TEST_F(SqliteMigrationsTest, TheFloorsAreRelease1sSchemas) {
  EXPECT_EQ(ProfileDbLadder().floor, 9) << "the signed-format cutover (SIGNED_FORMATS) moved it";
  EXPECT_EQ(ThreadDbLadder().floor, 4);
  EXPECT_GE(ProfileDbLadder().current, ProfileDbLadder().floor);
  EXPECT_GE(ThreadDbLadder().current, ThreadDbLadder().floor);
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
