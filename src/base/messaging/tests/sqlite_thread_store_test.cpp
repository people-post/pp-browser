#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ConversationSummaryCodec.h"
#include "base/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace pbr {
namespace {

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

} // namespace
} // namespace pbr
