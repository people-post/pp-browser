// The UI demo app's fixed fake data (DEV/PLAN-ui-demo-data.md), run on the real SQLite thread store, the roster
// store, the contacts file and a content store, in the normal build.
#include "lib/people/ContactIdentity.h"
#include "feature/product/DemoDataSeeder.h"

#include "common/Utilities.h"
#include "domain/attachments/AttachmentContent.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/content/testing/InMemoryObjectIndex.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/storage/SqliteGroupRosterStore.h"
#include "lib/storage/SqliteThreadSchema.h"
#include "lib/storage/SqliteThreadStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x40 + i);
  }
  return dek;
}

constexpr int64_t kSomeDay = 1'760'000'000'000LL;  // any instant; the seeder is told "today 00:00" explicitly

class DemoDataSeederTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_demo_seed_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    threads_ = std::make_unique<SqliteThreadStore>(dir_.string());
    ASSERT_TRUE(threads_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    groups_ = std::make_unique<SqliteGroupRosterStore>(SqliteThreadProfileDbFile(dir_.string()));
    content_ = MakeInMemoryIndexedCasStore(dir_.string(), "test");
  }
  void TearDown() override {
    threads_.reset();
    groups_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  DemoSeedDeps Deps(const int64_t midnight = kSomeDay) {
    return DemoSeedDeps{*threads_, *contacts_, *groups_, *content_, dir_.string(), TestDek(), 64ULL * 1024 * 1024,
                        /*own_relay_user_id=*/"", midnight, midnight + 11 * 3600 * 1000, ReadDemoAsset};
  }

  /** Everything the seeder wrote, as text: equal text = equal data (ids, order, unread, times, payloads). */
  std::string Inventory() {
    std::ostringstream out;
    auto contacts = contacts_->List();
    EXPECT_TRUE(contacts);
    std::vector<Contact> sorted = *contacts;
    std::sort(sorted.begin(), sorted.end(), [](const Contact& a, const Contact& b) { return a.id < b.id; });
    for (const Contact& c : sorted) {
      out << "C " << c.id << "|" << c.remote.nickname << "|" << c.remote.fetched_at << "|" << c.remote.ids.size() << "|"
          << c.remote.multiaddrs.size() << "\n";
    }
    std::vector<Thread> list = Threads();
    std::sort(list.begin(), list.end(), [](const Thread& a, const Thread& b) { return a.id < b.id; });
    for (const Thread& t : list) {
      out << "T " << t.id << "|" << t.title << "|" << static_cast<int>(t.kind) << "|" << t.unread_count << "|"
          << t.updated_at << "|" << t.preview << "|" << t.peer_identity_value << "\n";
      auto messages = threads_->GetMessages(t.id);
      EXPECT_TRUE(messages);
      for (const ThreadMessage& m : *messages) {
        out << "  M " << m.id << "|" << m.display_order << "|" << m.sender_contact_id << "|"
            << static_cast<int>(m.content_type) << "|" << m.text << "|" << m.payload_json << "|" << m.timestamp << "|"
            << static_cast<int>(m.delivery) << "|" << m.relay_visible << "\n";
      }
    }
    for (const char* group : {"demo-group-1", "demo-group-2"}) {
      auto members = groups_->ListMembers(group);
      EXPECT_TRUE(members);
      out << "G " << group << " " << members->size() << "\n";
    }
    return out.str();
  }

  std::vector<Thread> Threads() {
    auto threads = threads_->ListThreads();
    EXPECT_TRUE(threads);
    return *threads;
  }

  void MarkHeavyThreadRead(std::string* id = nullptr) {
    for (const Thread& t : Threads()) {
      if (t.unread_count == 120) {
        if (id != nullptr) {
          *id = t.id;
        }
        Thread read = t;
        read.unread_count = 0;
        ASSERT_TRUE(threads_->UpsertThread(read));
      }
    }
  }

  std::filesystem::path dir_;
  std::unique_ptr<SqliteThreadStore> threads_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<SqliteGroupRosterStore> groups_;
  std::shared_ptr<CasStore> content_;
};

TEST_F(DemoDataSeederTest, SeedsContactsThreadsUnreadCountsAndTwoDifferentPictures) {
  DemoDataSeeder seeder;
  auto seeded = seeder.SeedOnce(Deps());
  ASSERT_TRUE(seeded) << seeded.error().message;
  EXPECT_TRUE(*seeded);

  auto contacts = contacts_->List();
  ASSERT_TRUE(contacts);
  EXPECT_EQ(contacts->size(), 18u);
  for (const Contact& c : *contacts) {
    ASSERT_EQ(c.remote.ids.size(), 1u) << c.id;  // only the local-only demo account id
    EXPECT_EQ(c.remote.ids[0].value, "demo-acct-" + c.id);
    EXPECT_FALSE(ContactRelayUserId(c).has_value()) << c.id;
    EXPECT_TRUE(c.remote.multiaddrs.empty());
    EXPECT_GT(c.remote.fetched_at, 0);
  }

  size_t direct = 0, group = 0, ai = 0;
  std::set<int> unread;
  std::vector<ThreadMessage> pictures;
  for (const Thread& t : Threads()) {
    auto messages = threads_->GetMessages(t.id);
    ASSERT_TRUE(messages);
    EXPECT_FALSE(messages->empty()) << t.id;
    unread.insert(t.unread_count);
    switch (t.kind) {
    case ThreadKind::Direct:
      ++direct;
      EXPECT_EQ(t.peer_identity_value.rfind("demo-acct-", 0), 0u) << t.id;
      break;
    case ThreadKind::Group:
      ++group;
      break;
    case ThreadKind::Ai:
      ++ai;
      break;
    }
    for (const ThreadMessage& m : *messages) {
      if (m.content_type == ChatContentType::Attachment && t.kind == ThreadKind::Direct) {
        pictures.push_back(m);
      }
    }
  }
  EXPECT_EQ(direct, 11u);  // ten with contacts, one with someone who is not a contact (Home's New people)
  EXPECT_EQ(group, 2u);
  EXPECT_EQ(ai, 1u);
  EXPECT_TRUE(unread.count(0) && unread.count(3) && unread.count(120));

  auto roster1 = groups_->ListMembers("demo-group-1");
  auto roster2 = groups_->ListMembers("demo-group-2");
  ASSERT_TRUE(roster1 && roster2);
  EXPECT_EQ(roster1->size(), 5u);  // the user, three contacts and one member who is not a contact
  EXPECT_EQ(roster2->size(), 12u);
  auto metadata = groups_->LoadMetadata("demo-group-1");
  ASSERT_TRUE(metadata && metadata->has_value());
  EXPECT_EQ((*metadata)->sync_state, GroupSyncState::Active);

  // Two picture messages, both in the content store, with different hashes.
  ASSERT_EQ(pictures.size(), 2u);
  std::set<ByteVector> hashes;
  for (const ThreadMessage& m : pictures) {
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(m.payload_json);
    ASSERT_TRUE(fields) << fields.error().message;
    EXPECT_TRUE(fields->url.empty());
    EXPECT_TRUE(AttachmentBlobExists(*content_, fields->content_hash));
    auto bytes = LoadAttachmentPlaintext(*content_, fields->content_hash, TestDek());
    ASSERT_TRUE(bytes);
    EXPECT_EQ(bytes->size(), fields->byte_length);
    hashes.insert(fields->content_hash);
  }
  EXPECT_EQ(hashes.size(), 2u);
}

TEST_F(DemoDataSeederTest, FirstGroupShowsReactionsPicturesAFileAVideoAndANonContact) {
  DemoDataSeeder seeder;
  ASSERT_TRUE(seeder.SeedOnce(Deps()));
  auto messages = threads_->GetMessages("demo-thread-group-1");
  ASSERT_TRUE(messages);
  std::set<std::string> mimes;
  size_t reactions = 0;
  bool stranger = false;
  for (const ThreadMessage& m : *messages) {
    if (m.content_type == ChatContentType::Annotation) {
      ++reactions;
      ASSERT_TRUE(m.target_message_id.has_value()) << m.id;
      EXPECT_EQ(m.target_message_id->rfind("demo-msg-g1-", 0), 0u) << m.id;
    }
    if (m.content_type == ChatContentType::Attachment) {
      auto fields = ChatPayloadCodec::DecodeAttachmentJson(m.payload_json);
      ASSERT_TRUE(fields) << fields.error().message;
      EXPECT_TRUE(AttachmentBlobExists(*content_, fields->content_hash)) << m.id;
      mimes.insert(fields->mime);
    }
    stranger = stranger || m.sender_contact_id == "demo-acct-guest-01";
  }
  EXPECT_GE(reactions, 5u);
  EXPECT_EQ(mimes, (std::set<std::string>{"image/png", "application/pdf", "video/mp4"}));
  EXPECT_TRUE(stranger);
}

TEST_F(DemoDataSeederTest, NoMessageIsInTheFutureAndTheOrderIsKept) {
  // "now" is 09:00 on the demo day, so any "today 18:20" line would be in the future.
  DemoSeedDeps deps = Deps();
  deps.now_ms = deps.today_midnight_ms + 9 * 3600 * 1000;
  DemoDataSeeder seeder;
  ASSERT_TRUE(seeder.SeedOnce(deps));
  for (const Thread& t : Threads()) {
    EXPECT_LE(t.updated_at, deps.now_ms);
    auto messages = threads_->GetMessages(t.id);
    ASSERT_TRUE(messages);
    int64_t previous = 0;
    for (const ThreadMessage& m : *messages) {
      EXPECT_LE(m.timestamp, deps.now_ms) << m.id;
      EXPECT_GE(m.timestamp, previous) << m.id;
      previous = m.timestamp;
    }
  }
}

TEST_F(DemoDataSeederTest, NothingIsPendingAndTheOutboxIsEmpty) {
  DemoDataSeeder seeder;
  ASSERT_TRUE(seeder.SeedOnce(Deps()));
  for (const Thread& t : Threads()) {
    auto messages = threads_->GetMessages(t.id);
    ASSERT_TRUE(messages);
    for (const ThreadMessage& m : *messages) {
      EXPECT_NE(m.delivery, MessageDelivery::Pending) << m.id;
      if (m.sender_contact_id == kLocalSelfContactId) {
        EXPECT_FALSE(m.relay_visible) << m.id;
      }
    }
  }
  auto outbox = threads_->ListPendingOutbox();
  ASSERT_TRUE(outbox);
  EXPECT_TRUE(outbox->empty());
}

TEST_F(DemoDataSeederTest, NothingCanBeRoutedOrQueued) {
  DemoDataSeeder seeder;
  ASSERT_TRUE(seeder.SeedOnce(Deps()));
  // Direct chats carry a local-only account id (so the list shows the contact's name). No contact has a relay user
  // id, a peer id or an address, so a send has no route: delivery marks it failed (never pending), and the demo app
  // has no relay or mesh anyway. A group send stops at "Local relay identity unavailable" (no relay user id).
  auto contacts = contacts_->List();
  ASSERT_TRUE(contacts);
  ASSERT_EQ(contacts->size(), 18u);
  for (const Contact& c : *contacts) {
    EXPECT_FALSE(ContactRelayUserId(c).has_value()) << c.id;
    EXPECT_TRUE(c.remote.multiaddrs.empty()) << c.id;
    EXPECT_TRUE(c.remote.endpoints.empty()) << c.id;
    for (const mesh::ContactId& id : c.remote.ids) {
      EXPECT_EQ(id.kind, mesh::ContactIdKind::Account) << c.id;
    }
  }
  auto outbox = threads_->ListPendingOutbox();
  ASSERT_TRUE(outbox);
  EXPECT_TRUE(outbox->empty());
}

TEST_F(DemoDataSeederTest, ARegisteredAccountGetsNothing) {
  DemoSeedDeps deps = Deps();
  deps.own_relay_user_id = "relay:real-user";
  DemoDataSeeder seeder;
  auto seeded = seeder.SeedOnce(deps);
  ASSERT_TRUE(seeded);
  EXPECT_FALSE(*seeded);
  EXPECT_TRUE(Threads().empty());
  auto contacts = contacts_->List();
  ASSERT_TRUE(contacts);
  EXPECT_TRUE(contacts->empty());
  auto groups = groups_->ListGroupIds();
  ASSERT_TRUE(groups);
  EXPECT_TRUE(groups->empty());
}

TEST_F(DemoDataSeederTest, AnUnreadablePictureStopsBeforeAnyRowIsWritten) {
  DemoSeedDeps deps = Deps();
  deps.read_asset = [](const std::string&) -> Roe<ByteVector> { return Error("no such asset"); };
  DemoDataSeeder seeder;
  EXPECT_FALSE(seeder.SeedOnce(deps));
  EXPECT_TRUE(Threads().empty());
  auto contacts = contacts_->List();
  ASSERT_TRUE(contacts);
  EXPECT_TRUE(contacts->empty());
}

TEST_F(DemoDataSeederTest, StartingAgainRestoresTheSameInventoryEvenAfterAChatWasRead) {
  {
    DemoDataSeeder first_start;
    ASSERT_TRUE(first_start.SeedOnce(Deps()));
  }
  const std::string before = Inventory();

  std::string heavy_id;
  MarkHeavyThreadRead(&heavy_id);
  ASSERT_FALSE(heavy_id.empty());
  EXPECT_NE(Inventory(), before);

  DemoDataSeeder second_start;  // a new process: the once flag is new
  auto seeded = second_start.SeedOnce(Deps());
  ASSERT_TRUE(seeded);
  EXPECT_TRUE(*seeded);
  EXPECT_EQ(Inventory(), before);
  auto heavy = threads_->GetThread(heavy_id);
  ASSERT_TRUE(heavy && heavy->has_value());
  EXPECT_EQ((*heavy)->unread_count, 120);
}

TEST_F(DemoDataSeederTest, TheReadyCallbackFiringTwiceWritesOnce) {
  DemoDataSeeder seeder;
  ASSERT_TRUE(seeder.SeedOnce(Deps()));
  MarkHeavyThreadRead();
  const std::string after_read = Inventory();
  auto second = seeder.SeedOnce(Deps());
  ASSERT_TRUE(second);
  EXPECT_FALSE(*second);
  EXPECT_EQ(Inventory(), after_read);  // the count was not reset
}

TEST_F(DemoDataSeederTest, EveryTimeIsAFixedOffsetFromTodayMidnight) {
  constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;
  const auto offsets = [&](const int64_t midnight) {
    std::map<std::string, int64_t> out;
    for (const Thread& t : Threads()) {
      auto messages = threads_->GetMessages(t.id);
      EXPECT_TRUE(messages);
      for (const ThreadMessage& m : *messages) {
        out[m.id] = m.timestamp - midnight;
      }
      out["thread:" + t.id] = t.updated_at - midnight;
    }
    return out;
  };

  DemoDataSeeder first;
  ASSERT_TRUE(first.SeedOnce(Deps(kSomeDay)));
  const auto at_first_day = offsets(kSomeDay);
  ASSERT_FALSE(at_first_day.empty());
  for (const auto& [id, offset] : at_first_day) {
    EXPECT_GE(offset, -31 * kDayMs) << id;
    EXPECT_LT(offset, kDayMs) << id;  // never later than the end of today
  }

  // Another "today": the same rows move with it, so the screens show the same relative times.
  constexpr int64_t kLater = kSomeDay + 5 * kDayMs;
  DemoDataSeeder second;
  ASSERT_TRUE(second.SeedOnce(Deps(kLater)));
  EXPECT_EQ(offsets(kLater), at_first_day);
}

TEST(DemoPeerReplyTest, AnswersInTheMessagesLanguage) {
  EXPECT_EQ(DemoPeerReply("see you at 8"), "Got it! (an automatic reply in the demo app)");
  EXPECT_EQ(DemoPeerReply("晚上八点见"), "收到！（演示版的自动回复）");
}

TEST(LocalMidnightTest, IsAtMostOneDayBeforeTheInstantAndOnAWholeLocalDay) {
  const int64_t midnight = LocalMidnightMs(kSomeDay);
  EXPECT_LE(midnight, kSomeDay);
  EXPECT_GT(midnight, kSomeDay - 25LL * 60 * 60 * 1000);
  EXPECT_EQ(LocalMidnightMs(midnight), midnight);
}

} // namespace
} // namespace pbr
