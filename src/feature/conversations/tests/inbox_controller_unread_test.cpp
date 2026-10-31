#include "lib/messaging/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "common/Utilities.h"
#include "domain/peers/PeerDisplaySources.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

struct InboxTestEnv {
  explicit InboxTestEnv(const std::string& suffix) {
    data_dir = std::filesystem::temp_directory_path() / ("pp_browser_inbox_" + suffix);
    std::filesystem::remove_all(data_dir);
    store = std::make_unique<SqliteThreadStore>(data_dir.string());
    if (!store->SetDek(TestDek())) {
      throw std::runtime_error("Failed to set test DEK");
    }
    contacts = std::make_unique<ContactsStore>(data_dir.string());
    shadows = std::make_unique<DirectoryShadowCache>(directory);
    labels = std::make_unique<PeerDisplayResolver>(*contacts, PeerDisplaySourcesFrom(*shadows));
    inbox = std::make_unique<InboxController>(*store, *contacts, *labels, shadows.get());
  }

  std::filesystem::path data_dir;
  MockDirectoryClient directory;
  std::unique_ptr<SqliteThreadStore> store;
  std::unique_ptr<ContactsStore> contacts;
  std::unique_ptr<DirectoryShadowCache> shadows;
  std::unique_ptr<PeerDisplayResolver> labels;
  std::unique_ptr<InboxController> inbox;
};

TEST(InboxControllerUnreadTest, IncrementSumAndMarkRead) {
  InboxTestEnv env("unread_test");
  ASSERT_TRUE(env.inbox->ListThreads());

  Thread direct;
  direct.id = "thread-direct-1";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.participant_contact_ids = {"contact-1"};
  direct.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(direct));

  env.inbox->IncrementUnread(direct.id);
  env.inbox->IncrementUnread(direct.id);

  auto loaded = env.store->GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 2);
  EXPECT_EQ(env.inbox->SumUnread(), 2);
  EXPECT_EQ(env.inbox->SumUnreadForContact("contact-1"), 2);
  EXPECT_EQ(env.inbox->SumUnreadForContact("missing"), 0);

  ASSERT_TRUE(env.inbox->OpenThread(direct.id));
  loaded = env.store->GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 0);
  EXPECT_EQ(env.inbox->SumUnread(), 0);
}

TEST(InboxControllerUnreadTest, ActiveThreadSkipsInboundUnread) {
  InboxTestEnv env("unread_active_test");
  ASSERT_TRUE(env.inbox->ListThreads());

  Thread direct;
  direct.id = "thread-direct-active";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(direct));

  ASSERT_TRUE(env.inbox->OpenThread(direct.id));
  env.inbox->OnInboundMessagePersisted(direct.id, "hello");

  auto loaded = env.store->GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 0);
  EXPECT_EQ(loaded->value().preview, "hello");
}

TEST(InboxControllerUnreadTest, AnAnswerToTheActiveThreadCountsWhenItsPageIsNotShown) {
  // Compact layout: back on the list, the thread is still "active" but its page is closed; an AI answer that
  // lands there must show as unread (even_if_active). Without the flag the active thread is skipped as before.
  InboxTestEnv env("unread_even_if_active_test");
  ASSERT_TRUE(env.inbox->ListThreads());
  Thread ai;
  ai.id = "thread-ai-active";
  ai.kind = ThreadKind::Ai;
  ai.title = "AI";
  ai.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(ai));
  ASSERT_TRUE(env.inbox->OpenThread(ai.id));

  env.inbox->IncrementUnread(ai.id);
  EXPECT_EQ((*env.store->GetThread(ai.id))->unread_count, 0);
  env.inbox->IncrementUnread(ai.id, 1, /*even_if_active=*/true);
  EXPECT_EQ((*env.store->GetThread(ai.id))->unread_count, 1);
  ASSERT_TRUE(env.inbox->OpenThread(ai.id)); // opening it again reads it
  EXPECT_EQ((*env.store->GetThread(ai.id))->unread_count, 0);
}

TEST(InboxControllerUnreadTest, InactiveThreadIncrementsOnInbound) {
  InboxTestEnv env("unread_inbound_test");
  ASSERT_TRUE(env.inbox->ListThreads());

  Thread direct;
  direct.id = "thread-direct-inbound";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(direct));

  env.inbox->OnInboundMessagePersisted(direct.id, "ping");

  auto loaded = env.store->GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 1);
  EXPECT_EQ(loaded->value().preview, "ping");
}

TEST(InboxControllerTest, CreateClearCloseLeavesNoForcedAiHome) {
  InboxTestEnv env("ai_session_lifecycle");

  auto listed = env.inbox->ListThreads();
  ASSERT_TRUE(listed);
  EXPECT_TRUE(listed->empty());
  EXPECT_TRUE(env.inbox->ActiveThreadId().empty());

  auto created = env.inbox->CreateNewAiThread();
  ASSERT_TRUE(created);
  EXPECT_EQ(created->kind, ThreadKind::Ai);
  EXPECT_EQ(env.inbox->ActiveThreadId(), created->id);

  env.inbox->ClearActiveThread();
  EXPECT_TRUE(env.inbox->ActiveThreadId().empty());

  ASSERT_TRUE(env.inbox->CloseThread(created->id));
  listed = env.inbox->ListThreads();
  ASSERT_TRUE(listed);
  EXPECT_TRUE(listed->empty());
  EXPECT_TRUE(env.inbox->ActiveThreadId().empty());
}

// G016: a dissolved group's chat is deleted like any chat (nobody is left to tell); the line shows in the group only.
TEST(InboxControllerTest, ADissolvedGroupChatReadsWithItsLineAndDeletes) {
  InboxTestEnv env("dissolved_group_chat");
  auto group = env.store->FindOrCreateGroupThread("group:hike", "Weekend hike", {});
  ASSERT_TRUE(group);
  ThreadMessage said;
  said.id = "m1";
  said.thread_id = group->id;
  said.sender_contact_id = "account:carol";
  said.content_type = ChatContentType::Text;
  said.text = "see you at eight";
  said.timestamp = 1;
  ASSERT_TRUE(env.store->AppendMessage(said));
  const std::string detail = GroupMembershipCodec::EncodeGroupDissolved("group:hike", "account:alice").value();
  auto line = GroupMembershipCodec::BuildSystemMessage(group->id, GroupMembershipControlType::GroupDissolved,
                                                       "Group dissolved", detail, "account:alice");
  ASSERT_TRUE(line);
  ASSERT_TRUE(env.store->AppendMessage(*line));

  // The same control message that reached the owner's direct chat is stored there too, and never shown.
  DirectChatTarget target;
  target.peer_identity_kind = "account";
  target.peer_identity_value = "account:alice";
  target.channel = ThreadChannel::E2ePublic;
  auto dm = env.store->FindOrCreateDirectThread(target, "", "Alice");
  ASSERT_TRUE(dm);
  auto wire = GroupMembershipCodec::BuildSystemMessage(dm->id, GroupMembershipControlType::GroupDissolved,
                                                       "The group owner dissolved this group", detail, "account:alice");
  ASSERT_TRUE(wire);
  ASSERT_TRUE(env.store->AppendMessage(*wire));

  const auto group_rows = env.inbox->BuildTranscript(group->id, std::nullopt, std::nullopt);
  ASSERT_EQ(group_rows.size(), 2u);
  EXPECT_EQ(GroupMembershipCodec::GroupDissolvedOwner(group_rows.back().message), "account:alice");
  EXPECT_TRUE(env.inbox->BuildTranscript(dm->id, std::nullopt, std::nullopt).empty());

  ASSERT_TRUE(env.inbox->CloseThread(group->id));
  EXPECT_FALSE(env.store->FindGroupThread("group:hike")->has_value());
}

} // namespace
} // namespace pbr
