// Me → Storage's "chats" share: the bytes the message databases (profile.db, every chat's thread.db) take on disk.
#include "lib/storage/ProfileStorage.h"
#include "lib/storage/SqliteThreadSchema.h"

#include "common/Utilities.h"
#include "foundation/crypto/CryptoConstants.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace pbr {
namespace {

TEST(MessageStoreBytesTest, CountsTheProfileDatabaseAndEveryChatsDatabase) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / ("pp_storage_bytes_" + util::GenerateUuid());
  {
    auto stores = OpenSqliteProfileStores(data_dir.string(), "p1");
    ASSERT_TRUE(stores);
    ASSERT_TRUE(stores->message_store_bytes);
    for (IDekConsumer* at_rest : stores->at_rest) {
      ASSERT_TRUE(at_rest->SetDek(ByteVector(kDataEncryptionKeySize, 0x31)));
    }
    const uint64_t empty = stores->message_store_bytes();
    EXPECT_GT(empty, 0u) << "profile.db exists from the start";

    Thread thread;
    thread.id = "chat-1";
    thread.kind = ThreadKind::Direct;
    thread.title = "Chat";
    ASSERT_TRUE(stores->threads->UpsertThread(thread));
    for (int i = 0; i < 50; ++i) {
      ThreadMessage message;
      message.id = "m" + std::to_string(i);
      message.thread_id = thread.id;
      message.sender_contact_id = "alice";
      message.text = std::string(400, 'x');
      message.timestamp = i;
      ASSERT_TRUE(stores->threads->AppendMessage(message));
    }
    stores->threads->Flush();
    ASSERT_TRUE(std::filesystem::exists(SqliteThreadDbFile(data_dir.string(), thread.id)));
    EXPECT_GT(stores->message_store_bytes(), empty) << "the chat's own database counts";
  }
  std::error_code ec;
  std::filesystem::remove_all(data_dir, ec);
}

} // namespace
} // namespace pbr
