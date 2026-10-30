#include "lib/messaging/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "domain/peers/PeerDisplaySources.h"

#include "lib/messaging/GroupMentions.h"
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


// --- "Someone mentioned you" marker (G015) ------------------------------------------------------------

constexpr const char* kSelf = "acct-me";
constexpr const char* kOwner = "acct-owner";

std::vector<GroupRosterMember> MentionRoster(const bool include_self = true) {
  std::vector<GroupRosterMember> roster;
  GroupRosterMember owner;
  owner.member_identity = kOwner;
  owner.role = MemberRole::Owner;
  roster.push_back(owner);
  if (include_self) {
    GroupRosterMember self;
    self.member_identity = kSelf;
    roster.push_back(self);
  }
  return roster;
}

Thread GroupThread(const std::string& id) {
  Thread group;
  group.id = id;
  group.kind = ThreadKind::Group;
  group.group_id = "group:" + id;
  group.title = "Team";
  group.updated_at = 1;
  return group;
}

ThreadMessage MentionMessage(const std::string& thread_id, const std::string& sender, const bool mention_me) {
  ThreadMessage message;
  message.id = "m-" + thread_id;
  message.thread_id = thread_id;
  message.sender_contact_id = sender;
  message.text = "hi @Me";
  MentionsPayload mentions;
  if (mention_me) {
    mentions.mentions.push_back({kSelf, 3, 6});
  }
  message.payload_json = BuildMentionsPayloadJson(message.text, mentions);
  return message;
}

bool MentionFlag(InboxTestEnv& env, const std::string& id) {
  auto loaded = env.store->GetThread(id);
  EXPECT_TRUE(loaded && *loaded);
  return loaded && *loaded && (*loaded)->unread_mention;
}

void WireMention(InboxTestEnv& env, std::vector<GroupRosterMember>* roster) {
  env.inbox->SetMentionContext([] { return std::string(kSelf); },
                               [roster](const std::string&) { return *roster; });
}

TEST(InboxControllerMentionTest, LiveMentionSetsMarkerAndOpeningClearsIt) {
  InboxTestEnv env("mention_live");
  auto roster = MentionRoster();
  WireMention(env, &roster);
  const Thread group = GroupThread("g1");
  ASSERT_TRUE(env.store->UpsertThread(group));

  const ThreadMessage plain = MentionMessage("g1", kOwner, false);
  env.inbox->OnInboundMessagePersisted("g1", "hi", &plain, true);
  EXPECT_FALSE(MentionFlag(env, "g1"));

  const ThreadMessage mention = MentionMessage("g1", kOwner, true);
  env.inbox->OnInboundMessagePersisted("g1", "hi", &mention, true);
  EXPECT_TRUE(MentionFlag(env, "g1"));

  ASSERT_TRUE(env.inbox->OpenThread("g1"));
  EXPECT_FALSE(MentionFlag(env, "g1"));
}

TEST(InboxControllerMentionTest, HistorySyncNeverSetsTheMarker) {
  InboxTestEnv env("mention_history");
  auto roster = MentionRoster();
  WireMention(env, &roster);
  ASSERT_TRUE(env.store->UpsertThread(GroupThread("g1")));
  const ThreadMessage mention = MentionMessage("g1", kOwner, true);
  env.inbox->OnInboundMessagePersisted("g1", std::nullopt, &mention, false);
  EXPECT_FALSE(MentionFlag(env, "g1"));
}

TEST(InboxControllerMentionTest, OpenThreadIsNotMarked) {
  InboxTestEnv env("mention_open");
  auto roster = MentionRoster();
  WireMention(env, &roster);
  ASSERT_TRUE(env.store->UpsertThread(GroupThread("g1")));
  ASSERT_TRUE(env.inbox->OpenThread("g1"));
  const ThreadMessage mention = MentionMessage("g1", kOwner, true);
  env.inbox->OnInboundMessagePersisted("g1", "hi", &mention, true);
  EXPECT_FALSE(MentionFlag(env, "g1"));
}

TEST(InboxControllerMentionTest, NotOnTheRosterIsNotMentioned) {
  InboxTestEnv env("mention_roster");
  auto roster = MentionRoster(false);
  WireMention(env, &roster);
  ASSERT_TRUE(env.store->UpsertThread(GroupThread("g1")));
  const ThreadMessage mention = MentionMessage("g1", kOwner, true);
  env.inbox->OnInboundMessagePersisted("g1", "hi", &mention, true);
  EXPECT_FALSE(MentionFlag(env, "g1"));
  EXPECT_FALSE(env.inbox->MessageMentionsMe("group:g1", mention));
  roster = MentionRoster(true);
  EXPECT_TRUE(env.inbox->MessageMentionsMe("group:g1", mention));
}

TEST(InboxControllerMentionTest, MarkerSurvivesReopeningTheStore) {
  InboxTestEnv env("mention_persist");
  auto roster = MentionRoster();
  WireMention(env, &roster);
  ASSERT_TRUE(env.store->UpsertThread(GroupThread("g1")));
  const ThreadMessage mention = MentionMessage("g1", kOwner, true);
  env.inbox->OnInboundMessagePersisted("g1", "hi", &mention, true);
  env.store->Flush();

  SqliteThreadStore reopened(env.data_dir.string());
  ASSERT_TRUE(reopened.SetDek(TestDek()));
  auto loaded = reopened.GetThread("g1");
  ASSERT_TRUE(loaded && *loaded);
  EXPECT_TRUE((*loaded)->unread_mention);
}

} // namespace
} // namespace pbr
