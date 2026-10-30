// Who a group message's mentions reach (group-chat G015): checked against the local roster on arrival.
#include "lib/messaging/MentionAlert.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/messaging/TextSpans.h"

#include "common/Utilities.h"

#include <filesystem>
#include <memory>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char kGroup[] = "group:hike";
constexpr const char kOwner[] = "account:alice";
constexpr const char kBob[] = "account:bob";
constexpr const char kMe[] = "account:carol";

class MentionAlertTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_mention_alert_" + util::GenerateUuid());
    store_ = std::make_unique<SqliteThreadStore>(dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    roster_ = std::make_unique<GroupRosterStore>(store_->ProfileDbPath());
    GroupMetadata metadata;
    metadata.group_id = kGroup;
    metadata.owner_identity = kOwner;
    metadata.roster_epoch = 1;
    ASSERT_TRUE(roster_->UpsertMetadata(metadata));
    for (const char* member : {kOwner, kBob, kMe}) {
      GroupRosterMember row;
      row.member_identity = member;
      row.role = std::string(member) == kOwner ? MemberRole::Owner : MemberRole::Member;
      ASSERT_TRUE(roster_->UpsertMember(kGroup, row));
    }
  }

  void TearDown() override {
    roster_.reset();
    store_.reset();
    std::filesystem::remove_all(dir_);
  }

  ThreadMessage From(const std::string& sender, const std::string& text, const std::vector<TextSpan>& spans) {
    ThreadMessage message;
    message.content_type = ChatContentType::Text;
    message.sender_contact_id = sender;
    message.text = text;
    message.payload_json = text_spans::ToPayloadJson(text, spans);
    return message;
  }

  bool Reaches(const ThreadMessage& message, bool blocked = false) {
    return MessageMentionsUs(message, kGroup, kMe, *roster_, blocked);
  }

  void SetSyncState(GroupSyncState state) {
    GroupMetadata metadata = roster_->LoadMetadata(kGroup).value().value();
    metadata.sync_state = state;
    ASSERT_TRUE(roster_->UpsertMetadata(metadata));
  }

  static TextSpan MentionOf(const std::string& account) {
    return TextSpan{.kind = static_cast<uint8_t>(TextSpanKind::Mention), .start = 0, .end = 2, .account_id = account};
  }
  static TextSpan Everyone() { return TextSpan{.kind = static_cast<uint8_t>(TextSpanKind::Everyone), .start = 0, .end = 9}; }

  std::filesystem::path dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<GroupRosterStore> roster_;
};

TEST_F(MentionAlertTest, AMentionOfUsReachesUs) {
  EXPECT_TRUE(Reaches(From(kBob, "@C hi", {MentionOf(kMe)})));
  EXPECT_FALSE(Reaches(From(kBob, "@A hi", {MentionOf(kOwner)}))) << "someone else mentioned";
  EXPECT_FALSE(Reaches(From(kBob, "plain", {}))) << "no mention at all";
  EXPECT_FALSE(Reaches(From(kMe, "@C note to self", {MentionOf(kMe)}))) << "our own message";
  EXPECT_FALSE(Reaches(From(kBob, "@C hi", {MentionOf(kMe)}), /*blocked=*/true)) << "a blocked sender";
  EXPECT_FALSE(Reaches(From("account:stranger", "@C hi", {MentionOf(kMe)}))) << "not a member";
  EXPECT_FALSE(MessageMentionsUs(From(kBob, "@C hi", {MentionOf(kMe)}), "", kMe, *roster_, false))
      << "a direct chat: mentions show plain";
}

TEST_F(MentionAlertTest, EveryoneIsTheOwnersOnly) {
  EXPECT_TRUE(Reaches(From(kOwner, "@everyone dinner", {Everyone()})));
  EXPECT_FALSE(Reaches(From(kBob, "@everyone spam", {Everyone()}))) << "a member cannot reach the whole group";
}

TEST_F(MentionAlertTest, NotWhileTheGroupIsCheckingOrAfterWeWereRemoved) {
  SetSyncState(GroupSyncState::Checking);
  EXPECT_FALSE(Reaches(From(kBob, "@C hi", {MentionOf(kMe)}))) << "the roster may be stale (G013)";
  SetSyncState(GroupSyncState::Removed);
  EXPECT_FALSE(Reaches(From(kOwner, "@everyone", {Everyone()})));
}

TEST(MutedMentionAlertTest, AtMostOnceAMinutePerGroup) {
  EXPECT_TRUE(MutedMentionMayAlert(0, 1'000'000)) << "the first one";
  EXPECT_FALSE(MutedMentionMayAlert(1'000'000, 1'000'000 + kMutedMentionAlertGapMs - 1));
  EXPECT_TRUE(MutedMentionMayAlert(1'000'000, 1'000'000 + kMutedMentionAlertGapMs));
}

} // namespace
} // namespace pbr
