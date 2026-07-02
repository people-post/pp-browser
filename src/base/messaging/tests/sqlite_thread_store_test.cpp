#include "base/messaging/ChatPayloadCodec.h"
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
