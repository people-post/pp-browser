#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/ConversationSummaryCodec.h"
#include "lib/messaging/EditMessage.h"
#include "lib/storage/SqliteThreadStore.h"

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

void AssertStoreUnlocked(SqliteThreadStore& store) {
  ASSERT_TRUE(store.SetDek(TestDek()));
}

TEST(SqliteThreadStoreTest, AiThreadSurvivesRestartAndClearHistory) {
  const std::filesystem::path data_dir = std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_store_test";
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

// The AI chat screen looks one message up by id (a chat button's entry, the answer it stores): GetMessage finds it
// in its own thread, the oldest one too, without loading the thread's pages.
TEST(SqliteThreadStoreTest, GetMessageFindsOneMessageByIdInItsThreadOnly) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_store_get_message_test";
  std::filesystem::remove_all(data_dir);
  {
    SqliteThreadStore store(data_dir.string());
    AssertStoreUnlocked(store);
    for (const char* id : {"thread-a", "thread-b"}) {
      Thread thread;
      thread.id = id;
      thread.kind = ThreadKind::Ai;
      thread.updated_at = 1;
      ASSERT_TRUE(store.UpsertThread(thread));
    }
    const auto append = [&store](const std::string& thread_id, const std::string& id, const std::string& text) {
      ThreadMessage message;
      message.id = id;
      message.thread_id = thread_id;
      message.sender_contact_id = kAiAssistantContactId;
      message.text = text;
      message.timestamp = 42;
      ASSERT_TRUE(store.AppendMessage(message));
    };
    append("thread-a", "a1", "oldest answer");
    append("thread-a", "a2", "second");
    append("thread-a", "a3", "newest");
    append("thread-b", "b1", "elsewhere");

    auto found = store.GetMessage("thread-a", "a1");
    ASSERT_TRUE(found);
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->text, "oldest answer");
    EXPECT_EQ((*found)->thread_id, "thread-a");

    auto other = store.GetMessage("thread-a", "b1");
    ASSERT_TRUE(other);
    EXPECT_FALSE(other->has_value()) << "a message of another thread";
    auto unknown = store.GetMessage("thread-a", "no-such-id");
    ASSERT_TRUE(unknown);
    EXPECT_FALSE(unknown->has_value());
  }
  std::filesystem::remove_all(data_dir);
}

// "Delete for me": the content goes, the id stays (so a resend still dedupes), AI context and history
// serving no longer see the row.
TEST(SqliteThreadStoreTest, DeleteMessageContentKeepsAnIdOnlyTombstone) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_store_delete_test";
  std::filesystem::remove_all(data_dir);
  {
    SqliteThreadStore store(data_dir.string());
    AssertStoreUnlocked(store);
    Thread thread;
    thread.id = "thread-ai-del";
    thread.kind = ThreadKind::Ai;
    thread.title = "AI";
    ASSERT_TRUE(store.UpsertThread(thread));
    for (int i = 1; i <= 2; ++i) {
      ThreadMessage message;
      message.id = "msg-" + std::to_string(i);
      message.thread_id = thread.id;
      message.sender_contact_id = kLocalSelfContactId;
      message.text = "secret " + std::to_string(i);
      message.timestamp = i;
      ASSERT_TRUE(store.AppendMessage(message));
    }

    auto deleted = store.DeleteMessageContent(thread.id, "msg-1");
    ASSERT_TRUE(deleted);
    EXPECT_TRUE(*deleted);
    auto again = store.DeleteMessageContent(thread.id, "msg-1"); // idempotent
    ASSERT_TRUE(again);
    EXPECT_TRUE(*again);
    auto unknown = store.DeleteMessageContent(thread.id, "msg-9");
    ASSERT_TRUE(unknown);
    EXPECT_FALSE(*unknown);

    auto has = store.HasMessageId(thread.id, "msg-1");
    ASSERT_TRUE(has);
    EXPECT_TRUE(*has);
    auto page = store.GetMessagesPage(thread.id, std::nullopt, 100);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 2u);
    EXPECT_EQ(page->front().id, "msg-1");
    EXPECT_EQ(page->front().content_type, ChatContentType::Deleted);
    EXPECT_TRUE(page->front().text.empty());
    EXPECT_TRUE(page->front().payload_json.empty());
    EXPECT_EQ(page->back().text, "secret 2");
    auto eligible = store.CountContextEligibleMessagesAfter(thread.id, 0);
    ASSERT_TRUE(eligible);
    EXPECT_EQ(*eligible, 1);
    auto exported = store.ExportMessagesUpTo(thread.id, std::nullopt);
    ASSERT_TRUE(exported);
    ASSERT_EQ(exported->size(), 1u);
    EXPECT_EQ(exported->front().id, "msg-2");
    auto bounded = store.ExportMessagesUpTo(thread.id, std::string("msg-1")); // the bound is the deleted one
    ASSERT_TRUE(bounded);
    EXPECT_TRUE(bounded->empty());
    store.Flush();
  }
  std::filesystem::remove_all(data_dir);
}

// An edit rewrites the row in place (DEV/PLAN-message-edit.md): after a restart the row reads the new text and its edit
// mark, in the same place; nothing new in the table layout (the mark is in the encrypted payload_json).
TEST(SqliteThreadStoreTest, AnEditedMessageReadsBackAfterARestart) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_store_edit_test";
  std::filesystem::remove_all(data_dir);
  Thread thread;
  thread.id = "thread-direct-edit";
  thread.kind = ThreadKind::Direct;
  thread.title = "Bob";
  ThreadMessage original;
  {
    SqliteThreadStore store(data_dir.string());
    AssertStoreUnlocked(store);
    ASSERT_TRUE(store.UpsertThread(thread));
    ThreadMessage message;
    message.id = "msg-1";
    message.thread_id = thread.id;
    message.sender_contact_id = "account:bob";
    message.text = "see you at eight";
    message.timestamp = 1000;
    message.sender_seq = 3;
    message.session_epoch = 1;
    message.delivery = MessageDelivery::Relayed;
    original = *store.AppendMessage(message);
    ThreadMessage later = message;
    later.id = "msg-2";
    later.text = "bring snacks";
    later.timestamp = 2000;
    later.sender_seq = 4;
    ASSERT_TRUE(store.AppendMessage(later));
    ThreadMessage edit;
    edit.id = "edit-1";
    edit.thread_id = thread.id;
    edit.sender_contact_id = "account:bob";
    edit.content_type = ChatContentType::Annotation;
    edit.target_message_id = "msg-1";
    edit.text = "see you at nine";
    edit.payload_json = BuildEditPayloadJson("msg-1", "see you at nine");
    edit.timestamp = 1500;
    const Thread before_edit = store.GetThread(thread.id).value().value();
    ASSERT_TRUE(store.AppendMessage(edit));
    const Thread after_edit = store.GetThread(thread.id).value().value();
    EXPECT_EQ(after_edit.updated_at, before_edit.updated_at) << "an edit row does not move the chat";
    EXPECT_EQ(after_edit.preview, "bring snacks") << "nor become its preview";
    auto updated = store.UpdateMessage(WithEdit(*store.GetMessage(thread.id, "msg-1").value(), edit));
    ASSERT_TRUE(updated);
    ASSERT_TRUE(*updated);
    store.Flush();
  }
  {
    SqliteThreadStore store(data_dir.string());
    AssertStoreUnlocked(store);
    auto found = store.GetMessage(thread.id, "msg-1");
    ASSERT_TRUE(found);
    ASSERT_TRUE(found->has_value());
    const ThreadMessage& row = **found;
    EXPECT_EQ(row.text, "see you at nine");
    EXPECT_EQ(row.content_type, ChatContentType::Text);
    ASSERT_TRUE(EditMarkOf(row).has_value());
    EXPECT_EQ(EditMarkOf(row)->edited_at, 1500);
    EXPECT_EQ(EditMarkOf(row)->edited_by, "edit-1");
    EXPECT_EQ(row.display_order, original.display_order);
    EXPECT_EQ(row.timestamp, 1000);
    EXPECT_EQ(row.sender_seq, std::optional<uint64_t>(3));
    auto page = store.GetMessagesPage(thread.id, std::nullopt, 100);
    ASSERT_TRUE(page);
    ASSERT_FALSE(page->empty());
    EXPECT_EQ(page->front().id, "msg-1") << "it keeps its place";
    store.Flush();
  }
  std::filesystem::remove_all(data_dir);
}

/** Blanks the edit row right after the next read of it (`blank_next_`): a void between a read and a write. */
class BlankAfterReadStore final : public SqliteThreadStore {
public:
  using SqliteThreadStore::SqliteThreadStore;
  Roe<std::optional<ThreadMessage>> GetMessage(const std::string& thread_id,
                                               const std::string& message_id) const override {
    auto found = SqliteThreadStore::GetMessage(thread_id, message_id);
    if (blank_next_ && found && found->has_value()) {
      blank_next_ = false;
      ThreadMessage blanked = BlankedEdit(**found);
      blanked.thread_id = thread_id;
      (void)const_cast<BlankAfterReadStore*>(this)->UpdateMessage(blanked);
    }
    return found;
  }
  mutable bool blank_next_ = false;
};

TEST(SqliteThreadStoreTest, ADeliveryMarkNeverWritesBackWordsBlankedWhileItRan) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_store_delivery_race_test";
  std::filesystem::remove_all(data_dir);
  BlankAfterReadStore store(data_dir.string());
  AssertStoreUnlocked(store);
  Thread thread;
  thread.id = "thread-direct-race";
  thread.kind = ThreadKind::Direct;
  thread.title = "Bob";
  ASSERT_TRUE(store.UpsertThread(thread));
  ThreadMessage edit;
  edit.id = "edit-1";
  edit.thread_id = thread.id;
  edit.sender_contact_id = kLocalSelfContactId;
  edit.content_type = ChatContentType::Annotation;
  edit.target_message_id = "msg-1";
  edit.text = "words since deleted";
  edit.payload_json = BuildEditPayloadJson("msg-1", "words since deleted");
  edit.timestamp = 1500;
  edit.relay_visible = true;
  ASSERT_TRUE(store.AppendMessage(edit));
  store.blank_next_ = true;
  ASSERT_TRUE(store.UpdateMessageDelivery(thread.id, "edit-1", MessageDelivery::Relayed, MessageTransport::Relay,
                                          /*unless_delivered=*/false));
  if (store.blank_next_) { // the mark read nothing: the void lands just after it instead
    store.blank_next_ = false;
    ThreadMessage blanked = BlankedEdit(*store.GetMessage(thread.id, "edit-1").value());
    blanked.thread_id = thread.id;
    ASSERT_TRUE(store.UpdateMessage(blanked));
  }
  const ThreadMessage row = *store.GetMessage(thread.id, "edit-1").value();
  EXPECT_TRUE(row.text.empty()) << "the blanked words stay gone";
  EXPECT_EQ(EditTextOf(row), std::optional<std::string>(""));
  EXPECT_EQ(row.delivery, MessageDelivery::Relayed);
  store.Flush();
  std::filesystem::remove_all(data_dir);
}

// A send's outcome writes the row's delivery state and nothing else: an edit of ours blanked (voided) while its send
// was on the way stays blank when the "delivered" mark lands after it.
TEST(SqliteThreadStoreTest, ADeliveryMarkChangesOnlyTheDeliveryState) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_store_delivery_mark_test";
  std::filesystem::remove_all(data_dir);
  SqliteThreadStore store(data_dir.string());
  AssertStoreUnlocked(store);
  Thread thread;
  thread.id = "thread-direct-mark";
  thread.kind = ThreadKind::Direct;
  thread.title = "Bob";
  ASSERT_TRUE(store.UpsertThread(thread));
  ThreadMessage edit;
  edit.id = "edit-1";
  edit.thread_id = thread.id;
  edit.sender_contact_id = kLocalSelfContactId;
  edit.content_type = ChatContentType::Annotation;
  edit.target_message_id = "msg-1";
  edit.text = "words since deleted";
  edit.payload_json = BuildEditPayloadJson("msg-1", "words since deleted");
  edit.timestamp = 1500;
  edit.relay_visible = true;
  edit.delivery = MessageDelivery::Pending;
  ASSERT_TRUE(store.AppendMessage(edit));
  ThreadMessage blanked = BlankedEdit(*store.GetMessage(thread.id, "edit-1").value());
  blanked.thread_id = thread.id;
  ASSERT_TRUE(store.UpdateMessage(blanked));

  auto marked = store.UpdateMessageDelivery(thread.id, "edit-1", MessageDelivery::Relayed, MessageTransport::Relay,
                                            /*unless_delivered=*/false);
  ASSERT_TRUE(marked);
  EXPECT_TRUE(*marked);
  const ThreadMessage row = *store.GetMessage(thread.id, "edit-1").value();
  EXPECT_TRUE(row.text.empty()) << "the blanked words stay gone";
  EXPECT_EQ(EditTextOf(row), std::optional<std::string>(""));
  EXPECT_EQ(row.content_type, ChatContentType::Annotation);
  EXPECT_EQ(row.delivery, MessageDelivery::Relayed);
  EXPECT_EQ(row.transport, std::optional<MessageTransport>(MessageTransport::Relay));

  // A failed copy (unless_delivered) leaves a delivered row alone; a missing transport keeps the stored one.
  marked = store.UpdateMessageDelivery(thread.id, "edit-1", MessageDelivery::Failed, std::nullopt, true);
  ASSERT_TRUE(marked);
  EXPECT_FALSE(*marked);
  EXPECT_EQ(store.GetMessage(thread.id, "edit-1").value()->delivery, MessageDelivery::Relayed);
  marked = store.UpdateMessageDelivery(thread.id, "edit-1", MessageDelivery::Failed, std::nullopt, false);
  ASSERT_TRUE(marked);
  EXPECT_TRUE(*marked);
  EXPECT_EQ(store.GetMessage(thread.id, "edit-1").value()->delivery, MessageDelivery::Failed);
  EXPECT_EQ(store.GetMessage(thread.id, "edit-1").value()->transport,
            std::optional<MessageTransport>(MessageTransport::Relay));
  marked = store.UpdateMessageDelivery(thread.id, "unknown", MessageDelivery::Relayed, std::nullopt, false);
  ASSERT_TRUE(marked);
  EXPECT_FALSE(*marked);
  store.Flush();
  std::filesystem::remove_all(data_dir);
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
  const std::filesystem::path data_dir = std::filesystem::temp_directory_path() / "pp_browser_sqlite_thread_enc_test";
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
  ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT content_enc FROM messages WHERE id = ?;", -1, &stmt, nullptr), SQLITE_OK);
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

TEST(SqliteThreadStoreTest, OneUnreadableRowDoesNotBlankThePage) {
  const std::filesystem::path data_dir = std::filesystem::temp_directory_path() / "pp_browser_sqlite_bad_row_test";
  std::filesystem::remove_all(data_dir);
  Thread thread;
  thread.id = "thread-bad-row";
  thread.kind = ThreadKind::Group;
  thread.title = "Bad row";
  SqliteThreadStore store(data_dir.string());
  AssertStoreUnlocked(store);
  ASSERT_TRUE(store.UpsertThread(thread));
  for (const char* id : {"m1", "m2", "m3"}) {
    ThreadMessage message;
    message.id = id;
    message.thread_id = thread.id;
    message.sender_contact_id = kLocalSelfContactId;
    message.text = std::string("text ") + id;
    message.timestamp = 1;
    ASSERT_TRUE(store.AppendMessage(message));
  }
  store.Flush();

  // m2's sealed body is damaged (as an old or interrupted write might leave it).
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open_v2((data_dir / "threads" / thread.id / "thread.db").string().c_str(), &db,
                            SQLITE_OPEN_READWRITE, nullptr),
            SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(db, "UPDATE messages SET content_enc = x'0102030405' WHERE id = 'm2';", nullptr, nullptr, nullptr),
      SQLITE_OK);
  sqlite3_close(db);

  auto page = store.GetMessagesPage(thread.id, std::nullopt, 50);
  ASSERT_TRUE(page);
  ASSERT_EQ(page->size(), 2u) << "the rest of the page reads";
  EXPECT_EQ(page->front().id, "m1");
  EXPECT_EQ(page->back().id, "m3");

  // The row is not deleted: it is still there to be looked at.
  ASSERT_EQ(sqlite3_open_v2((data_dir / "threads" / thread.id / "thread.db").string().c_str(), &db,
                            SQLITE_OPEN_READONLY, nullptr),
            SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM messages;", -1, &stmt, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(stmt, 0), 3);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

TEST(SqliteThreadStoreTest, ListThreadsPreviewRoundTripEncrypted) {
  const std::filesystem::path data_dir = std::filesystem::temp_directory_path() / "pp_browser_sqlite_preview_enc_test";
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
  EXPECT_EQ("0100000000000000000548656c6c6f", [](const std::vector<uint8_t>& bytes) {
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
