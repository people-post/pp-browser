#include "base/messaging/SqliteThreadStore.h"
#include "base/people/ContactsStore.h"
#include "feature/messaging/InboxController.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(InboxControllerUnreadTest, IncrementSumAndMarkRead) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_inbox_unread_test";
  std::filesystem::remove_all(data_dir);

  SqliteThreadStore store(data_dir.string());
  ContactsStore contacts(data_dir.string());
  InboxController inbox(store, contacts);
  ASSERT_TRUE(inbox.ListThreads());

  Thread direct;
  direct.id = "thread-direct-1";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.participant_contact_ids = {"contact-1"};
  direct.updated_at = 1;
  ASSERT_TRUE(store.UpsertThread(direct));

  inbox.IncrementUnread(direct.id);
  inbox.IncrementUnread(direct.id);

  auto loaded = store.GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 2);
  EXPECT_EQ(inbox.SumUnread(), 2);
  EXPECT_EQ(inbox.SumUnreadForContact("contact-1"), 2);
  EXPECT_EQ(inbox.SumUnreadForContact("missing"), 0);

  ASSERT_TRUE(inbox.OpenThread(direct.id));
  loaded = store.GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 0);
  EXPECT_EQ(inbox.SumUnread(), 0);
}

TEST(InboxControllerUnreadTest, ActiveThreadSkipsInboundUnread) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_inbox_unread_active_test";
  std::filesystem::remove_all(data_dir);

  SqliteThreadStore store(data_dir.string());
  ContactsStore contacts(data_dir.string());
  InboxController inbox(store, contacts);
  ASSERT_TRUE(inbox.ListThreads());

  Thread direct;
  direct.id = "thread-direct-active";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.updated_at = 1;
  ASSERT_TRUE(store.UpsertThread(direct));

  ASSERT_TRUE(inbox.OpenThread(direct.id));
  inbox.OnInboundMessagePersisted(direct.id, "hello");

  auto loaded = store.GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 0);
  EXPECT_EQ(loaded->value().preview, "hello");
}

TEST(InboxControllerUnreadTest, InactiveThreadIncrementsOnInbound) {
  const std::filesystem::path data_dir =
      std::filesystem::temp_directory_path() / "pp_browser_inbox_unread_inbound_test";
  std::filesystem::remove_all(data_dir);

  SqliteThreadStore store(data_dir.string());
  ContactsStore contacts(data_dir.string());
  InboxController inbox(store, contacts);
  ASSERT_TRUE(inbox.ListThreads());

  Thread direct;
  direct.id = "thread-direct-inbound";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.updated_at = 1;
  ASSERT_TRUE(store.UpsertThread(direct));

  inbox.OnInboundMessagePersisted(direct.id, "ping");

  auto loaded = store.GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 1);
  EXPECT_EQ(loaded->value().preview, "ping");
}

} // namespace
} // namespace pbr
