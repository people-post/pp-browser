#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "common/Utilities.h"
#include "lib/people/ProfileIconCache.h"
#include "feature/conversations/PeerDisplaySources.h"

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
    store = std::make_unique<InMemoryDeliveryStore>();
    store->SetHiddenInDirectThread(&GroupMembershipCodec::IsHiddenInDirectThread); // as the app's storage does
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
  std::unique_ptr<InMemoryDeliveryStore> store;
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
  env.inbox->OnInboundMessagePersisted(direct.id, "hello", true);

  auto loaded = env.store->GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 0);
  EXPECT_EQ(loaded->value().preview, "hello");
}

TEST(InboxControllerUnreadTest, InboundUnreadFollowsOpenThreadAndAttention) {
  // The four combinations (open x looking): only "open and looking" is not unread.
  struct Case {
    bool open;
    bool attentive;
    int expected_unread;
  };
  const Case cases[] = {{true, true, 0}, {true, false, 1}, {false, true, 1}, {false, false, 1}};
  int index = 0;
  for (const Case& c : cases) {
    InboxTestEnv env("unread_attention_matrix_" + std::to_string(index++));
    ASSERT_TRUE(env.inbox->ListThreads());

    Thread direct;
    direct.id = "thread-direct-attention";
    direct.kind = ThreadKind::Direct;
    direct.title = "Peer";
    direct.updated_at = 1;
    ASSERT_TRUE(env.store->UpsertThread(direct));
    if (c.open) {
      ASSERT_TRUE(env.inbox->OpenThread(direct.id));
    }

    env.inbox->OnInboundMessagePersisted(direct.id, "hi", c.attentive);

    auto loaded = env.store->GetThread(direct.id);
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(*loaded);
    EXPECT_EQ((*loaded)->unread_count, c.expected_unread) << "open=" << c.open << " attentive=" << c.attentive;
    EXPECT_EQ(env.inbox->SumUnread(), c.expected_unread);
  }
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

TEST(InboxControllerUnreadTest, AiThreadsDoNotCountTowardSumUnread) {
  // Kenneth 2026-11-05: an AI conversation never shows as unread (the assistant's answer to your own question is not
  // a waiting message), so it must not add to the unread total that drives the nav badge.
  InboxTestEnv env("unread_ai_excluded_test");
  ASSERT_TRUE(env.inbox->ListThreads());
  Thread direct;
  direct.id = "thread-direct-x";
  direct.kind = ThreadKind::Direct;
  direct.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(direct));
  env.inbox->IncrementUnread(direct.id);
  Thread ai;
  ai.id = "thread-ai-x";
  ai.kind = ThreadKind::Ai;
  ai.updated_at = 2;
  ASSERT_TRUE(env.store->UpsertThread(ai));
  env.inbox->IncrementUnread(ai.id, 3, /*even_if_active=*/true);
  EXPECT_EQ((*env.store->GetThread(ai.id))->unread_count, 3);
  EXPECT_EQ(env.inbox->SumUnread(), 1) << "only the human chat counts; the AI thread's unread is excluded";
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

  env.inbox->OnInboundMessagePersisted(direct.id, "ping", true);

  auto loaded = env.store->GetThread(direct.id);
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(*loaded);
  EXPECT_EQ((*loaded)->unread_count, 1);
  EXPECT_EQ(loaded->value().preview, "ping");
}

TEST(InboxControllerUnreadTest, ClearingAChatsHistoryDropsItsMentionMarker) {
  InboxTestEnv env("clear_drops_mentions");
  ASSERT_TRUE(env.inbox->ListThreads());
  InMemoryThreadMentionsStore mentions;
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
  ASSERT_TRUE(mentions.Add(group.id, "m-1", 2000));
  ASSERT_TRUE(env.inbox->OpenThread(group.id)); // read: m-1 becomes the chat's read-mention row
  env.inbox->ClearActiveThread();
  ASSERT_TRUE(mentions.Add(group.id, "m-3", 3000));
  ASSERT_TRUE(mentions.Add(other.id, "m-2", 1000));
  ASSERT_EQ(*mentions.Threads(), (std::set<std::string>{group.id, other.id}));
  ASSERT_EQ(mentions.Latest().value()->message_id, "m-3");

  ASSERT_TRUE(env.inbox->ClearThreadHistory(group.id, /*forget_memory=*/false));
  EXPECT_EQ(*mentions.Threads(), (std::set<std::string>{other.id})) << "only the cleared chat loses its marker";
  EXPECT_FALSE(mentions.Oldest(group.id).value());
  // The cleared chat's read row (m-1, newer than m-2) went too: only the other chat's older mention is left.
  const auto latest = mentions.Latest().value();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->thread_id, other.id);
  EXPECT_EQ(latest->message_id, "m-2");
}

TEST(InboxControllerUnreadTest, DeletingAChatDropsItsReadMention) {
  InboxTestEnv env("close_drops_read_mention");
  ASSERT_TRUE(env.inbox->ListThreads());
  InMemoryThreadMentionsStore mentions;
  env.inbox->SetThreadMentions(&mentions);
  Thread group;
  group.id = "thread-group-close";
  group.kind = ThreadKind::Group;
  group.title = "Hike";
  group.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(group));
  ASSERT_TRUE(mentions.Add(group.id, "m-1", 1000));
  ASSERT_TRUE(env.inbox->OpenThread(group.id)); // read: m-1 becomes the chat's read-mention row
  ASSERT_TRUE(mentions.Threads()->empty());
  ASSERT_TRUE(mentions.Latest().value());

  ASSERT_TRUE(env.inbox->CloseThread(group.id));
  EXPECT_FALSE(mentions.Latest().value()) << "a deleted chat leaves no recent mention";
}

TEST(InboxControllerUnreadTest, DeletingTheNewestReadMentionDropsItsRow) {
  InboxTestEnv env("delete_drops_read_mention");
  ASSERT_TRUE(env.inbox->ListThreads());
  InMemoryThreadMentionsStore mentions;
  env.inbox->SetThreadMentions(&mentions);
  Thread group;
  group.id = "thread-group-delete";
  group.kind = ThreadKind::Group;
  group.title = "Hike";
  group.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(group));
  ASSERT_TRUE(mentions.Add(group.id, "m-1", 1000));
  ASSERT_TRUE(mentions.Add(group.id, "m-2", 2000));
  ASSERT_TRUE(env.inbox->OpenThread(group.id)); // read: the newest, m-2, is kept as the read-mention row
  ASSERT_EQ(mentions.Latest().value()->message_id, "m-2");

  ThreadMessage deleted;
  deleted.id = "m-2";
  deleted.thread_id = group.id;
  deleted.content_type = ChatContentType::Text;
  env.inbox->AfterMessageDeleted(group.id, deleted);
  EXPECT_FALSE(mentions.Latest().value()) << "the deleted mention was the chat's only remembered one";
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
  ASSERT_TRUE(
      env.store->AppendMessage(Event(direct, GroupMembershipControlType::MemberRemoved, direct.updated_at + 5000)));
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
  ASSERT_TRUE(
      env.store->AppendMessage(Event(direct, GroupMembershipControlType::GroupRenamed, direct.updated_at + 7000)));
  EXPECT_EQ((*env.inbox->ListThreads())[0].preview, "hi there");
}

TEST(InboxControllerUnreadTest, ARealChatNeverVanishesNotEvenClearedAndRenamed) {
  InboxTestEnv env("never_vanishes");
  ASSERT_TRUE(env.inbox->ListThreads());
  const Thread direct = EventChat(env, "account:bob");
  ASSERT_TRUE(env.store->AppendMessage(Said(direct, "m-1", "hello", direct.updated_at + 1000)));
  ASSERT_EQ(env.inbox->ListThreads()->size(), 1u);
  ASSERT_TRUE(env.inbox->ClearThreadHistory(direct.id, /*forget_memory=*/false));
  ASSERT_TRUE(
      env.store->AppendMessage(Event(direct, GroupMembershipControlType::GroupRenamed, direct.updated_at + 9000)));
  EXPECT_EQ(env.inbox->ListThreads()->size(), 1u) << "cleared, then only an event in it: still the user's chat";
}

TEST(InboxControllerUnreadTest, AnOpenedOrPinnedEmptyChatStaysListed) {
  InboxTestEnv env("opened_pinned");
  ASSERT_TRUE(env.inbox->ListThreads());
  const Thread started = EventChat(env, "account:started");
  const Thread pinned = EventChat(env, "account:pinned");
  const Thread untouched = EventChat(env, "account:untouched");
  EXPECT_TRUE(env.inbox->ListThreads()->empty());
  ASSERT_TRUE(env.inbox->OpenThread(started.id)); // the user started a chat with a contact
  env.inbox->ClearActiveThread();
  env.inbox->KeepListed(pinned.id); // what pinning does
  ASSERT_TRUE(
      env.store->AppendMessage(Event(started, GroupMembershipControlType::MemberLeft, started.updated_at + 100)));
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
    ASSERT_TRUE(
        env.store->AppendMessage(Said(direct, "m-" + std::to_string(i), "real " + std::to_string(i), 1000 + i)));
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

// The ask-AI panel's first question makes its AI chat without opening it: the chat on screen stays the open one.
TEST(InboxControllerTest, AnAiChatCanBeMadeWithoutOpeningIt) {
  InboxTestEnv env("ai_thread_not_opened");
  auto open = env.inbox->CreateNewAiThread();
  ASSERT_TRUE(open);
  auto made = env.inbox->CreateAiThread();
  ASSERT_TRUE(made);
  EXPECT_EQ(made->kind, ThreadKind::Ai);
  EXPECT_NE(made->id, open->id);
  EXPECT_EQ(env.inbox->ActiveThreadId(), open->id) << "the open chat did not change";
  auto listed = env.inbox->ListThreads();
  ASSERT_TRUE(listed);
  EXPECT_EQ(listed->size(), 2u);
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
  EXPECT_EQ(group_rows.back().message.sender_contact_id, "account:alice");
  EXPECT_TRUE(env.inbox->BuildTranscript(dm->id, std::nullopt, std::nullopt).empty());

  ASSERT_TRUE(env.inbox->CloseThread(group->id));
  EXPECT_FALSE(env.store->FindGroupThread("group:hike")->has_value());
}

// A group row's sender can be an account id of someone who is not a contact (Kenneth 2026-11-04: no avatar column).
TEST(InboxControllerUnreadTest, SenderIconResolvesANonContactAccountThroughTheIdentityCache) {
  InboxTestEnv env("sender_icon_test");
  const std::string profile = env.data_dir.string();
  env.inbox->SetProfileDataDir(profile);
  ProfileIconCacheMeta meta;
  ASSERT_TRUE(SaveProfileIconCache(profile, {1, 2, 3}, "jpg", meta, SanitizeProfileIconCacheKey("account:stranger")));
  const std::string expected = ProfileIconLocalPath(profile, SanitizeProfileIconCacheKey("account:stranger"));
  ASSERT_FALSE(expected.empty());

  const TranscriptFacts facts = env.inbox->Facts();
  EXPECT_EQ(facts.sender_icon("account:stranger"), expected);
  EXPECT_TRUE(facts.sender_icon("account:nobody").empty());
}

} // namespace
} // namespace pbr
