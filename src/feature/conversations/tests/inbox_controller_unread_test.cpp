#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "domain/peers/PeerDisplaySources.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <set>
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

TEST(InboxControllerUnreadTest, ClearingAChatsHistoryDropsItsMentionMarker) {
  InboxTestEnv env("clear_drops_mentions");
  ASSERT_TRUE(env.inbox->ListThreads());
  ThreadMentionsStore mentions(env.store->ProfileDbPath());
  env.inbox->SetThreadMentions(&mentions);

  Thread group;
  group.id = "thread-group-1";
  group.kind = ThreadKind::Group;
  group.title = "Hike";
  group.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(group));
  Thread other = group;
  other.id = "thread-group-2";
  ASSERT_TRUE(env.store->UpsertThread(other));
  ASSERT_TRUE(mentions.Add(group.id, "m-1", 1000));
  ASSERT_TRUE(mentions.Add(other.id, "m-2", 1000));
  ASSERT_EQ(*mentions.Threads(), (std::set<std::string>{group.id, other.id}));

  ASSERT_TRUE(env.inbox->ClearThreadHistory(group.id, /*forget_memory=*/false));
  EXPECT_EQ(*mentions.Threads(), (std::set<std::string>{other.id})) << "only the cleared chat loses its marker";
  EXPECT_FALSE(mentions.Oldest(group.id).value());
}

// A direct chat the group traffic made (store creation: not listed until it is the user's).
Thread EventChat(InboxTestEnv& env, const std::string& peer) {
  DirectChatTarget target;
  target.peer_identity_kind = "account";
  target.peer_identity_value = peer;
  target.channel = ThreadChannel::E2ePublic;
  auto direct = env.store->FindOrCreateDirectThread(target, "contact-" + peer, peer);
  EXPECT_TRUE(direct);
  return *direct;
}

ThreadMessage Event(const Thread& thread, const GroupMembershipControlType type, const int64_t at) {
  auto event = GroupMembershipCodec::BuildSystemMessage(thread.id, type, "Member removed", "{}", "account:bob");
  EXPECT_TRUE(event);
  event->timestamp = at;
  return *event;
}

ThreadMessage Said(const Thread& thread, const std::string& id, const std::string& text, const int64_t at) {
  ThreadMessage message;
  message.id = id;
  message.thread_id = thread.id;
  message.sender_contact_id = "account:bob";
  message.text = text;
  message.timestamp = at;
  return message;
}

TEST(InboxControllerUnreadTest, ADirectChatMadeOnlyByGroupEventsIsNotListedUntilItIsReadable) {
  InboxTestEnv env("control_only");
  ASSERT_TRUE(env.inbox->ListThreads());
  const Thread direct = EventChat(env, "account:bob");
  ASSERT_TRUE(env.store->AppendMessage(Event(direct, GroupMembershipControlType::MemberRemoved, direct.updated_at + 5000)));
  auto after = env.store->GetThread(direct.id);
  ASSERT_TRUE(after && *after);
  EXPECT_TRUE((*after)->preview.empty()) << "the event is not the preview";
  EXPECT_EQ((*after)->updated_at, direct.updated_at) << "and does not reorder the list";
  EXPECT_TRUE(env.inbox->ListThreads()->empty()) << "control-only: hidden";

  ASSERT_TRUE(env.store->AppendMessage(Said(direct, "hello-1", "hi there", direct.updated_at + 6000)));
  auto listed = env.inbox->ListThreads();
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 1u);
  EXPECT_EQ((*listed)[0].preview, "hi there");

  // A later event leaves the last readable message as the preview.
  ASSERT_TRUE(env.store->AppendMessage(Event(direct, GroupMembershipControlType::GroupRenamed, direct.updated_at + 7000)));
  EXPECT_EQ((*env.inbox->ListThreads())[0].preview, "hi there");
}

TEST(InboxControllerUnreadTest, ARealChatNeverVanishesNotEvenClearedAndRenamed) {
  InboxTestEnv env("never_vanishes");
  ASSERT_TRUE(env.inbox->ListThreads());
  const Thread direct = EventChat(env, "account:bob");
  ASSERT_TRUE(env.store->AppendMessage(Said(direct, "m-1", "hello", direct.updated_at + 1000)));
  ASSERT_EQ(env.inbox->ListThreads()->size(), 1u);
  ASSERT_TRUE(env.inbox->ClearThreadHistory(direct.id, /*forget_memory=*/false));
  ASSERT_TRUE(env.store->AppendMessage(Event(direct, GroupMembershipControlType::GroupRenamed, direct.updated_at + 9000)));
  EXPECT_EQ(env.inbox->ListThreads()->size(), 1u) << "cleared, then only an event in it: still the user's chat";
}

TEST(InboxControllerUnreadTest, AnOpenedOrPinnedEmptyChatStaysListed) {
  InboxTestEnv env("opened_pinned");
  ASSERT_TRUE(env.inbox->ListThreads());
  const Thread started = EventChat(env, "account:started");
  const Thread pinned = EventChat(env, "account:pinned");
  const Thread untouched = EventChat(env, "account:untouched");
  EXPECT_TRUE(env.inbox->ListThreads()->empty());
  ASSERT_TRUE(env.inbox->OpenThread(started.id));  // the user started a chat with a contact
  env.inbox->ClearActiveThread();
  env.inbox->KeepListed(pinned.id);  // what pinning does
  ASSERT_TRUE(env.store->AppendMessage(Event(started, GroupMembershipControlType::MemberLeft, started.updated_at + 100)));
  std::set<std::string> ids;
  const auto listed = env.inbox->ListThreads();
  ASSERT_TRUE(listed);
  for (const Thread& thread : *listed) {
    ids.insert(thread.id);
  }
  EXPECT_EQ(ids, (std::set<std::string>{started.id, pinned.id}));
  EXPECT_EQ(ids.count(untouched.id), 0u);
}

TEST(InboxControllerUnreadTest, ALongRunOfEventsOnTopOfHistoryStillShowsTheHistory) {
  InboxTestEnv env("long_run");
  ASSERT_TRUE(env.inbox->ListThreads());
  const Thread direct = EventChat(env, "account:bob");
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(env.store->AppendMessage(Said(direct, "m-" + std::to_string(i), "real " + std::to_string(i), 1000 + i)));
  }
  for (int i = 0; i < 150; ++i) {
    ASSERT_TRUE(env.store->AppendMessage(Event(direct, GroupMembershipControlType::GroupRenamed, 2000 + i)));
  }
  const auto rows = env.inbox->BuildTranscript(direct.id, std::nullopt);
  EXPECT_EQ(rows.size(), 10u) << "more than a page of events on top: the page is not blank";
  // A window asked for by the caller is exactly that window.
  EXPECT_TRUE(env.inbox->BuildTranscript(direct.id, 130).empty());
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

} // namespace
} // namespace pbr
