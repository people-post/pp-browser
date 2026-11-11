// #210 step 2: a message of ours that never went out is taken away when the user sends it again as a new one — the row,
// its "did not go out" lines and its outbox record — and nothing else ever is. Run on the app's SQLite store and on the
// in-memory one the capabilities' tests use, through the IThreadStore port the facade calls.
#include "lib/delivery/DeliveryNotice.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/storage/SqliteThreadStore.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>

namespace pbr {
namespace {

enum class Impl { Sqlite, Memory };

class UnsentMessageStoreTest : public ::testing::TestWithParam<Impl> {
protected:
  void SetUp() override {
    if (GetParam() == Impl::Sqlite) {
      dir_ = std::filesystem::temp_directory_path() / ("pp_unsent_store_" + util::GenerateUuid());
      std::filesystem::create_directories(dir_);
      auto sqlite = std::make_unique<SqliteThreadStore>(dir_.string());
      ASSERT_TRUE(sqlite->SetDek(ByteVector(32, 0x5a)));
      store_ = std::move(sqlite);
    } else {
      store_ = std::make_unique<InMemoryDeliveryStore>();
    }
    Thread thread;
    thread.id = "t1";
    thread.kind = ThreadKind::Direct;
    thread.channel = ThreadChannel::E2ePublic;
    thread.title = "Bob";
    thread.updated_at = 1;
    ASSERT_TRUE(store().UpsertThread(thread));
  }
  void TearDown() override {
    store_.reset();
    if (!dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
    }
  }

  IThreadStore& store() { return *store_; }

  ThreadMessage Message(const std::string& id, const std::string& sender, const MessageDelivery delivery,
                        const int64_t at) {
    ThreadMessage message;
    message.id = id;
    message.thread_id = "t1";
    message.sender_contact_id = sender;
    message.text = "hi " + id;
    message.timestamp = at;
    message.delivery = delivery;
    message.relay_visible = true;
    return message;
  }

  void Add(const ThreadMessage& message) { ASSERT_TRUE(store().AppendMessage(message)); }

  bool Has(const std::string& id) {
    auto present = store().HasMessageId("t1", id);
    return present && *present;
  }

  size_t Count() {
    auto page = store().GetMessagesPage("t1", std::nullopt, 1000);
    return page ? page->size() : 0;
  }

  bool InOutbox(const std::string& id) {
    auto outbox = store().ListPendingOutbox();
    return outbox && std::any_of(outbox->begin(), outbox->end(), [&](const auto& entry) { return entry.first == id; });
  }

  std::filesystem::path dir_;
  std::unique_ptr<IThreadStore> store_;
};

TEST_P(UnsentMessageStoreTest, TheRowItsNoticesAndItsOutboxRecordGo) {
  Add(Message("keep", kLocalSelfContactId, MessageDelivery::Relayed, 1));
  Add(Message("m1", kLocalSelfContactId, MessageDelivery::Pending, 2));
  Add(MakeSendFailureRow("t1", "m1", SendFailureWord(SendFailure::RelayUnreachable).c_str(), 3));
  Add(MakeSendFailureRow("t1", "m1", kGroupMembersMissedWire, 4));
  Add(Message("m2", kLocalSelfContactId, MessageDelivery::Failed, 5));
  ASSERT_TRUE(InOutbox("m1"));
  ASSERT_EQ(Count(), 5u);

  auto removed = store().RemoveUnsentMessage("t1", "m1");
  ASSERT_TRUE(removed);
  EXPECT_TRUE(*removed);
  EXPECT_FALSE(Has("m1"));
  EXPECT_FALSE(Has(SendFailureRowId("m1", "")));
  EXPECT_FALSE(Has(SendFailureRowId("m1", kGroupMembersMissedWire)));
  EXPECT_FALSE(InOutbox("m1"));
  auto gone = store().GetMessage("t1", "m1");
  ASSERT_TRUE(gone);
  EXPECT_FALSE(gone->has_value());
  EXPECT_EQ(Count(), 2u) << "nothing left in its place (no tombstone), the others stay";
  EXPECT_TRUE(Has("keep"));
  EXPECT_TRUE(Has("m2"));

  removed = store().RemoveUnsentMessage("t1", "m2");
  ASSERT_TRUE(removed);
  EXPECT_TRUE(*removed) << "a failed row too";
  EXPECT_EQ(Count(), 1u);
}

TEST_P(UnsentMessageStoreTest, ItRefusesADeliveredAReceivedSomeoneElsesAndAnUnknownRow) {
  Add(Message("sent", kLocalSelfContactId, MessageDelivery::Relayed, 1));
  Add(Message("received", "account:bob", MessageDelivery::Relayed, 2));
  Add(Message("theirs-failed", "account:carol", MessageDelivery::Failed, 3));
  ThreadMessage local = Message("local", kLocalSelfContactId, MessageDelivery::Pending, 4);
  local.relay_visible = false;
  Add(local);
  for (const char* id : {"sent", "received", "theirs-failed", "local", "no-such-id"}) {
    auto removed = store().RemoveUnsentMessage("t1", id);
    ASSERT_TRUE(removed) << id;
    EXPECT_FALSE(*removed) << id;
  }
  EXPECT_EQ(Count(), 4u);
  auto other_thread = store().RemoveUnsentMessage("t2", "sent");
  EXPECT_TRUE(!other_thread || !*other_thread);
}

// The old request ending late in success marks the row as MarkDelivery does: it looks the row up in the page and
// updates it only when found — and an update never inserts. The row stays gone and nothing comes back.
TEST_P(UnsentMessageStoreTest, ALateSuccessDoesNotBringTheRowBack) {
  Add(Message("m1", kLocalSelfContactId, MessageDelivery::Failed, 1));
  Add(Message("new", kLocalSelfContactId, MessageDelivery::Pending, 2));
  ThreadMessage old = *(*store().GetMessage("t1", "m1"));
  ASSERT_TRUE(*store().RemoveUnsentMessage("t1", "m1"));
  const size_t before = Count();

  auto page = store().GetMessagesPage("t1", std::nullopt, 10000);
  ASSERT_TRUE(page);
  EXPECT_TRUE(std::none_of(page->begin(), page->end(), [](const ThreadMessage& row) { return row.id == "m1"; }));
  old.delivery = MessageDelivery::Relayed;
  auto updated = store().UpdateMessage(old);
  EXPECT_TRUE(!updated || !*updated);
  auto gone = store().GetMessage("t1", "m1");
  ASSERT_TRUE(gone);
  EXPECT_FALSE(gone->has_value());
  EXPECT_EQ(Count(), before);
}

INSTANTIATE_TEST_SUITE_P(Impls, UnsentMessageStoreTest, ::testing::Values(Impl::Sqlite, Impl::Memory),
                         [](const ::testing::TestParamInfo<Impl>& info) {
                           return info.param == Impl::Sqlite ? std::string("Sqlite") : std::string("Memory");
                         });

} // namespace
} // namespace pbr
