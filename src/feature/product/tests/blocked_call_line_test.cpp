// DEV/PLAN-call-stranger-declined.md (c): one grey local line per caller for calls "who can call me" dropped.
#include "feature/product/MakeCallFunctionalPorts.h"

#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/PeerThreadLogic.h"
#include "foundation/i18n/LocalizationService.h"
#include "common/thread/ThreadChannel.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr const char* kCaller = "account:stranger";

BlockedCallInvite Blocked(const std::string& call_id) {
  BlockedCallInvite blocked;
  blocked.call_id = call_id;
  blocked.sender_identity = kCaller;
  blocked.audience = mesh::InboundAudience::ContactsOnly;
  return blocked;
}

class BlockedCallLineTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    auto thread = ResolveDirectThreadForPeer(store_, kCaller, "", "");
    ASSERT_TRUE(thread) << thread.error().message;
    thread_id_ = *thread;
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }

  std::vector<ThreadMessage> Rows() { return store_.GetMessages(thread_id_).value(); }

  static int64_t CountOf(const ThreadMessage& row) {
    const auto payload = TryParseObject(row.payload_json);
    const auto detail = payload ? TryParseObject(payload->getString("detail").value_or("")) : std::nullopt;
    return detail ? static_cast<int64_t>(detail->getNonNegInt("count").value_or(0)) : -1;
  }

  InMemoryDeliveryStore store_;
  std::string thread_id_;
};

TEST_F(BlockedCallLineTest, TheFirstDroppedCallIsOneLocalLineAndNoUnread) {
  auto words = RecordBlockedCallLine(store_, thread_id_, Blocked("call:1"), 1'000);
  ASSERT_TRUE(words);
  EXPECT_EQ(*words, std::optional<std::string>("Missed call (declined by your settings)")) << "the list's preview";
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].id, BlockedCallLineId(kCaller));
  EXPECT_EQ(rows[0].content_type, ChatContentType::System);
  EXPECT_EQ(rows[0].delivery, MessageDelivery::Local);
  EXPECT_FALSE(rows[0].relay_visible) << "never sent";
  EXPECT_EQ(rows[0].text, "Missed call (declined by your settings)") << "the preview's stand-in";
  EXPECT_EQ(rows[0].timestamp, 1'000);
  EXPECT_EQ(CountOf(rows[0]), 1);
  EXPECT_TRUE(IsLocalActionMessage(rows[0])) << "never part of what a model is sent";
  auto thread = store_.GetThread(thread_id_);
  ASSERT_TRUE(thread && *thread);
  EXPECT_EQ((*thread)->unread_count, 0);
}

TEST_F(BlockedCallLineTest, TheNextCallCountsInTheSameLine) {
  ASSERT_TRUE(RecordBlockedCallLine(store_, thread_id_, Blocked("call:1"), 1'000));
  auto words = RecordBlockedCallLine(store_, thread_id_, Blocked("call:2"), 5'000);
  ASSERT_TRUE(words);
  EXPECT_EQ(*words, std::optional<std::string>("2 missed calls (declined by your settings)"));
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(CountOf(rows[0]), 2);
  EXPECT_EQ(rows[0].timestamp, 5'000) << "the last call's time";
  auto thread = store_.GetThread(thread_id_);
  ASSERT_TRUE(thread && *thread);
  EXPECT_EQ((*thread)->unread_count, 0);
}

// The writer keeps nothing in memory: a second call with the same id is what a restart's redelivery looks like.
TEST_F(BlockedCallLineTest, TheSameCallDeliveredAgainIsNotCountedAgain) {
  ASSERT_TRUE(RecordBlockedCallLine(store_, thread_id_, Blocked("call:1"), 1'000));
  auto again = RecordBlockedCallLine(store_, thread_id_, Blocked("call:1"), 9'000);
  ASSERT_TRUE(again);
  EXPECT_FALSE(again->has_value());
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(CountOf(rows[0]), 1);
  EXPECT_EQ(rows[0].timestamp, 1'000);
}

TEST_F(BlockedCallLineTest, OnlyTheLastCallIdsAreKept) {
  for (size_t i = 0; i < kBlockedCallIdsKept + 5; ++i) {
    ASSERT_TRUE(RecordBlockedCallLine(store_, thread_id_, Blocked("call:" + std::to_string(i)), 1'000 + i));
  }
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(CountOf(rows[0]), static_cast<int64_t>(kBlockedCallIdsKept + 5));
  const auto payload = TryParseObject(rows[0].payload_json);
  const auto detail = payload ? TryParseObject(payload->getString("detail").value_or("")) : std::nullopt;
  ASSERT_TRUE(detail);
  const Array* ids = detail->getArray("call_ids");
  ASSERT_NE(ids, nullptr);
  EXPECT_EQ(ids->elements.size(), kBlockedCallIdsKept);
}

TEST_F(BlockedCallLineTest, AnExpiredInviteIsRecordedAtItsOwnTimeAndDoesNotBumpTheList) {
  BlockedCallInvite old = Blocked("call:old");
  old.expired = true;
  old.created_at_ms = 400;
  auto words = RecordBlockedCallLine(store_, thread_id_, old, 90'000);
  ASSERT_TRUE(words);
  EXPECT_FALSE(words->has_value()) << "the list is not moved up to now";
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].timestamp, 400);
  auto thread = store_.GetThread(thread_id_);
  ASSERT_TRUE(thread && *thread);
  EXPECT_EQ((*thread)->updated_at, 400) << "placed by the call's own time";
  EXPECT_EQ((*thread)->preview, "Missed call (declined by your settings)");
}

TEST_F(BlockedCallLineTest, AnOlderExpiredInviteNeverMovesTheLineBack) {
  ASSERT_TRUE(RecordBlockedCallLine(store_, thread_id_, Blocked("call:new"), 50'000));
  auto thread_before = store_.GetThread(thread_id_);
  ASSERT_TRUE(thread_before && *thread_before);
  BlockedCallInvite old = Blocked("call:old");
  old.expired = true;
  old.created_at_ms = 400;
  auto words = RecordBlockedCallLine(store_, thread_id_, old, 60'000);
  ASSERT_TRUE(words);
  EXPECT_FALSE(words->has_value());
  const auto rows = Rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(CountOf(rows[0]), 2);
  EXPECT_EQ(rows[0].timestamp, 50'000) << "the newer call's time stays";
  auto thread_after = store_.GetThread(thread_id_);
  ASSERT_TRUE(thread_after && *thread_after);
  EXPECT_EQ((*thread_after)->updated_at, (*thread_before)->updated_at) << "the list's order and time stay";
}

TEST_F(BlockedCallLineTest, AnUnreadableStoredLineStartsOverAtOne) {
  for (const std::string& payload :
       {std::string("not json"), std::string(R"({"control_type":"call_blocked","detail":"{}"})"),
        std::string(R"({"control_type":"call_blocked","detail":"{bad"})")}) {
    InMemoryDeliveryStore store;
    auto thread = ResolveDirectThreadForPeer(store, kCaller, "", "");
    ASSERT_TRUE(thread);
    ThreadMessage broken;
    broken.id = BlockedCallLineId(kCaller);
    broken.thread_id = *thread;
    broken.sender_contact_id = kLocalSelfContactId;
    broken.content_type = ChatContentType::System;
    broken.payload_json = payload;
    broken.timestamp = 100;
    ASSERT_TRUE(store.AppendMessage(broken));
    auto words = RecordBlockedCallLine(store, *thread, Blocked("call:1"), 2'000);
    ASSERT_TRUE(words) << payload;
    const auto rows = store.GetMessages(*thread).value();
    ASSERT_EQ(rows.size(), 1u) << payload;
    EXPECT_EQ(CountOf(rows[0]), 1) << payload;
    EXPECT_EQ(rows[0].timestamp, 2'000) << payload;
  }
}

// The line is in the chat a stranger's text lands in: inbound text auto-creates the public DM keyed by the
// sender's account (RelayReceivePipeline), and the calls' resolver finds that same chat — in either order.
TEST_F(BlockedCallLineTest, TheLineAndTheCallersTextShareOneChat) {
  const DirectChatTarget inbound{
      .peer_identity_kind = "account", .peer_identity_value = kCaller, .channel = ThreadChannel::E2ePublic};
  auto text_thread = store_.FindOrCreateDirectThread(inbound, "", "stranger");
  ASSERT_TRUE(text_thread);
  EXPECT_EQ(text_thread->id, thread_id_) << "line first, text after";

  const std::string other = "account:other";
  const DirectChatTarget other_inbound{
      .peer_identity_kind = "account", .peer_identity_value = other, .channel = ThreadChannel::E2ePublic};
  auto other_text = store_.FindOrCreateDirectThread(other_inbound, "", "other");
  ASSERT_TRUE(other_text);
  auto other_line = ResolveDirectThreadForPeer(store_, other, "", "");
  ASSERT_TRUE(other_line);
  EXPECT_EQ(*other_line, other_text->id) << "text first, line after";
}

TEST_F(BlockedCallLineTest, ThePreviewIsInTheReadersLanguage) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  auto words = RecordBlockedCallLine(store_, thread_id_, Blocked("call:1"), 1'000);
  ASSERT_TRUE(words && *words);
  EXPECT_EQ(**words, "未接来电（已按你的设置拒接）");
  words = RecordBlockedCallLine(store_, thread_id_, Blocked("call:2"), 2'000);
  ASSERT_TRUE(words && *words);
  EXPECT_EQ(**words, "2 次未接来电（已按你的设置拒接）");
}

} // namespace
} // namespace pbr
