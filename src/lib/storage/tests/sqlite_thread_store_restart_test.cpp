// A message's rich fields (an annotation's target, its type) come back from disk after a restart, encrypted at rest.
#include "lib/storage/SqliteThreadStore.h"

#include "common/Utilities.h"
#include "foundation/crypto/CryptoConstants.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x30 + i);
  }
  return dek;
}

TEST(SqliteThreadStoreRestartTest, RichPayloadMetadataSurvivesARestart) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / ("pp_storage_rich_meta_" + util::GenerateUuid());
  Thread thread;
  thread.id = "thread-rich-meta";
  thread.kind = ThreadKind::Ai;
  thread.title = "Rich";
  {
    SqliteThreadStore store(data_dir.string());
    ASSERT_TRUE(store.SetDek(TestDek()));
    ASSERT_TRUE(store.UpsertThread(thread));
    ThreadMessage message;
    message.id = "annotation-1";
    message.thread_id = thread.id;
    message.sender_contact_id = kLocalSelfContactId;
    message.content_type = ChatContentType::Annotation;
    message.text = "note";
    message.target_message_id = "target-1";
    message.timestamp = 99;
    ASSERT_TRUE(store.AppendMessage(message));
    store.Flush();
  }
  {
    SqliteThreadStore store(data_dir.string());
    ASSERT_TRUE(store.SetDek(TestDek()));
    auto page = store.GetMessagesPage(thread.id, std::nullopt, 10);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->size(), 1u);
    EXPECT_EQ(page->front().content_type, ChatContentType::Annotation);
    EXPECT_EQ(page->front().target_message_id, std::optional<std::string>("target-1"));
  }
  std::error_code ec;
  std::filesystem::remove_all(data_dir, ec);
}

} // namespace
} // namespace pbr
