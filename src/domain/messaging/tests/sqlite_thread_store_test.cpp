#include "domain/messaging/ChatPayloadCodec.h"
#include "domain/messaging/ConversationSummaryCodec.h"
#include "domain/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <sqlite3.h>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

void AssertStoreUnlocked(SqliteThreadStore& store) { ASSERT_TRUE(store.SetDek(TestDek())); }

TEST(SqliteThreadStoreTest, AiThreadSurvivesRestartAndClearHistory) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_store_test";
  std::filesystem::remove_all(data_dir);

  Thread thread;
  thread.id = "thread-ai-1";
  thread.kind = ThreadKind::Ai;
  thread.title = "AI";
  thread.updated_at = 1;

  {
    SqliteThreadStore store(data_dir.string());
    AssertStoreUnlocked(store);
    ASSERT_TRUE(store.UpsertThread(thread));
    ThreadMessage message;
    message.id = "msg-1";
    message.thread_id = thread.id;
    message.sender_contact_id = kLocalSelfContactId;
    message.text = "hello sqlite";
    message.timestamp = 42;
    auto appended = store.AppendMessage(message);
    ASSERT_TRUE(appended);
    EXPECT_GT(appended->display_order, 0);
    store.Flush();
  }

  SqliteThreadStore store(data_dir.string());
  AssertStoreUnlocked(store);
  auto page = store.GetMessagesPage(thread.id, std::nullopt, 100);
  ASSERT_TRUE(page);
  ASSERT_EQ(page->size(), 1u);
  EXPECT_EQ(page->front().text, "hello sqlite");

  ASSERT_TRUE(store.ClearMessages(thread.id, ClearMessagesOptions{}));
  page = store.GetMessagesPage(thread.id, std::nullopt, 100);
  ASSERT_TRUE(page);
  EXPECT_TRUE(page->empty());

  auto threads = store.ListThreads();
  ASSERT_TRUE(threads);
  ASSERT_EQ(threads->size(), 1u);
  EXPECT_TRUE(threads->front().preview.empty());
  EXPECT_EQ(threads->front().unread_count, 0);
}

TEST(SqliteThreadStoreTest, ThreadMemoryRoundTripAndForgetOnClear) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_memory_test";
  std::filesystem::remove_all(data_dir);

  Thread thread;
  thread.id = "thread-ai-memory";
  thread.kind = ThreadKind::Ai;
  thread.title = "Memory";

  SqliteThreadStore store(data_dir.string());
  AssertStoreUnlocked(store);
  ASSERT_TRUE(store.UpsertThread(thread));

  ConversationSummary summary;
  summary.schema_version = 1;
  summary.version = 1;
  summary.text = "User prefers concise answers.";
  summary.compacted_through_display_order = 10;
  summary.updated_at = 1234;
  ASSERT_TRUE(store.SetThreadMemory(thread.id, summary));

  auto loaded = store.GetThreadMemory(thread.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ(loaded->value().text, summary.text);
  EXPECT_EQ(loaded->value().version, 1);
  EXPECT_EQ(loaded->value().compacted_through_display_order, 10);

  ThreadMessage message;
  message.id = "msg-before";
  message.thread_id = thread.id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = "old";
  message.display_order = 5;
  message.timestamp = 1;
  ASSERT_TRUE(store.AppendMessage(message));

  message.id = "msg-after";
  message.text = "recent";
  message.display_order = 20;
  ASSERT_TRUE(store.AppendMessage(message));

  const ContextBudget budget{};
  auto context = store.GetMessagesForContext(thread.id, budget);
  ASSERT_TRUE(context);
  ASSERT_EQ(context->size(), 1u);
  EXPECT_EQ(context->front().text, "recent");

  ASSERT_TRUE(store.ClearMessages(thread.id, ClearMessagesOptions{}));
  loaded = store.GetThreadMemory(thread.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(loaded->has_value());

  ASSERT_TRUE(store.ClearMessages(thread.id, ClearMessagesOptions{.forget_memory = true}));
  loaded = store.GetThreadMemory(thread.id);
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->has_value());
}

TEST(SqliteThreadStoreTest, MessageBodyIsEncryptedOnDisk) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_enc_test";
  std::filesystem::remove_all(data_dir);

  Thread thread;
  thread.id = "thread-enc";
  thread.kind = ThreadKind::Ai;
  thread.title = "Enc";

  SqliteThreadStore store(data_dir.string());
  AssertStoreUnlocked(store);
  ASSERT_TRUE(store.UpsertThread(thread));

  ThreadMessage message;
  message.id = "msg-secret";
  message.thread_id = thread.id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = "super secret plaintext";
  message.timestamp = 1;
  ASSERT_TRUE(store.AppendMessage(message));
  store.Flush();

  const std::filesystem::path thread_db = data_dir / "threads" / thread.id / "thread.db";
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open_v2(thread_db.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT content_enc FROM messages WHERE id = ?;", -1, &stmt, nullptr),
            SQLITE_OK);
  sqlite3_bind_text(stmt, 1, message.id.c_str(), -1, SQLITE_TRANSIENT);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  const void* blob = sqlite3_column_blob(stmt, 0);
  const int blob_size = sqlite3_column_bytes(stmt, 0);
  ASSERT_GT(blob_size, 0);
  const std::string_view on_disk(static_cast<const char*>(blob), static_cast<size_t>(blob_size));
  EXPECT_EQ(on_disk.find(message.text), std::string_view::npos);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

TEST(SqliteThreadStoreTest, ListThreadsPreviewRoundTripEncrypted) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_preview_enc_test";
  std::filesystem::remove_all(data_dir);

  Thread thread;
  thread.id = "thread-preview";
  thread.kind = ThreadKind::Ai;
  thread.title = "Preview";
  thread.preview = "sidebar snippet";

  SqliteThreadStore store(data_dir.string());
  AssertStoreUnlocked(store);
  ASSERT_TRUE(store.UpsertThread(thread));

  auto threads = store.ListThreads();
  ASSERT_TRUE(threads);
  ASSERT_EQ(threads->size(), 1u);
  EXPECT_EQ(threads->front().preview, "sidebar snippet");
}

TEST(ConversationSummaryCodecTest, RoundTripMatchesSchema) {
  ConversationSummary summary;
  summary.schema_version = ConversationSummaryCodec::kSchemaVersion;
  summary.version = 2;
  summary.text = "Merged facts.";
  summary.compacted_through_display_order = 99;
  summary.updated_at = 5678;

  auto encoded = ConversationSummaryCodec::Encode(summary);
  ASSERT_TRUE(encoded);
  auto decoded = ConversationSummaryCodec::Decode(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->text, summary.text);
  EXPECT_EQ(decoded->version, 2);
  EXPECT_EQ(decoded->compacted_through_display_order, 99);
}

TEST(ChatPayloadCodecTest, VectorATextRoundTrip) {
  const auto encoded = ChatPayloadCodec::EncodeText("Hello");
  ASSERT_TRUE(encoded);
  EXPECT_EQ(
      "0100000000000000000548656c6c6f",
      [](const std::vector<uint8_t>& bytes) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        for (const uint8_t byte : bytes) {
          out.push_back(kHex[byte >> 4]);
          out.push_back(kHex[byte & 0x0f]);
        }
        return out;
      }(*encoded));

  ThreadMessage message;
  ASSERT_TRUE(ChatPayloadCodec::ApplyRowToMessage(*encoded, message));
  EXPECT_EQ(message.text, "Hello");
}

class IncompatibleProfileDbTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / "pp_browser_sqlite_profile_version_guard_test";
    std::filesystem::remove_all(data_dir_);
  }

  void TearDown() override {
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
};

TEST_F(IncompatibleProfileDbTest, FailsEveryCallNotJustTheFirst) {
  {
    auto bootstrap = std::make_unique<SqliteThreadStore>(data_dir_.string());
    AssertStoreUnlocked(*bootstrap);
    ASSERT_TRUE(bootstrap->ListThreads());
    bootstrap->Flush();
    bootstrap.reset();
  }

  const std::filesystem::path profile_db = data_dir_ / "threads" / "profile.db";
  ASSERT_TRUE(std::filesystem::exists(profile_db));
  {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(profile_db.string().c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA user_version = 1;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
  }

  store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
  AssertStoreUnlocked(*store_);

  auto first = store_->ListThreads();
  ASSERT_FALSE(first);
  EXPECT_NE(first.error().message.find("Incompatible"), std::string::npos);

  // The guard must fire on every call. A failed open still assigned profile_db_,
  // so the early return in OpenProfileDb() skipped the version check from the
  // second call on and the store went on to read an incompatible database.
  auto second = store_->ListThreads();
  ASSERT_FALSE(second);
  EXPECT_NE(second.error().message.find("Incompatible"), std::string::npos);
}

} // namespace
} // namespace pbr
