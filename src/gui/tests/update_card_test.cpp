// The Chats list's "new version" card: when it shows (the recovery card's rhythm, per announced version), what "Later"
// remembers on the device, and that ChatController draws and answers it from the advice client-compat gives.
#include "feature/settings/UpdatePrefs.h"
#include "foundation/data/SessionStore.h"
#include "gui/UpdateCard.h"
#include "gui/chat/ChatController.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'800'000'000'000;

TEST(UpdateCardTest, ShowsWhileANewerVersionExistsAndNeverWithoutOne) {
  EXPECT_TRUE(ShouldShowUpdateCard("1.2.0", {}, kNow));
  EXPECT_FALSE(ShouldShowUpdateCard("", {}, kNow));
}

TEST(UpdateCardTest, LaterHidesItThenItReturnsOnceAfterSevenDaysThenNeverForThatVersion) {
  UpdateCardState state = UpdateCardAfterLater("1.2.0", {}, kNow);
  EXPECT_EQ(state.dismissals, 1);
  EXPECT_FALSE(ShouldShowUpdateCard("1.2.0", state, kNow + 1000));
  EXPECT_FALSE(ShouldShowUpdateCard("1.2.0", state, kNow + kRecoveryReminderRepeatMs - 1));
  EXPECT_TRUE(ShouldShowUpdateCard("1.2.0", state, kNow + kRecoveryReminderRepeatMs));
  EXPECT_EQ(kRecoveryReminderRepeatMs, 7LL * 24 * 60 * 60 * 1000);

  state = UpdateCardAfterLater("1.2.0", state, kNow + kRecoveryReminderRepeatMs);
  EXPECT_EQ(state.dismissals, 2);
  EXPECT_FALSE(ShouldShowUpdateCard("1.2.0", state, kNow + 100 * kRecoveryReminderRepeatMs));
  EXPECT_EQ(UpdateCardAfterLater("1.2.0", state, kNow).dismissals, 2) << "capped";
}

TEST(UpdateCardTest, AStillNewerVersionStartsOver) {
  UpdateCardState state = UpdateCardAfterLater("1.2.0", {}, kNow);
  state = UpdateCardAfterLater("1.2.0", state, kNow + kRecoveryReminderRepeatMs);
  ASSERT_FALSE(ShouldShowUpdateCard("1.2.0", state, kNow + 100 * kRecoveryReminderRepeatMs));
  EXPECT_TRUE(ShouldShowUpdateCard("1.3.0", state, kNow + 1)) << "announced again at once";
  const UpdateCardState next = UpdateCardAfterLater("1.3.0", state, kNow + 2);
  EXPECT_EQ(next.version, "1.3.0");
  EXPECT_EQ(next.dismissals, 1) << "the count belongs to the new version";
}

// The comparison is the one that decides the card (CompareSemverCore): a leading "v" is ignored, a pre-release suffix
// is not looked at.
TEST(UpdateCardTest, VersionsAreComparedNotTextAndALeadingVIsTheSameVersion) {
  const UpdateCardState later = UpdateCardAfterLater("1.2.0", {}, kNow);
  EXPECT_FALSE(ShouldShowUpdateCard("v1.2.0", later, kNow + 1000)) << "v1.2.0 is the announcement 1.2.0";
  EXPECT_FALSE(ShouldShowUpdateCard("1.2.0+build5", later, kNow + 1000));
  const UpdateCardState again = UpdateCardAfterLater("v1.2.0", later, kNow + 2000);
  EXPECT_EQ(again.version, "1.2.0") << "the stored text stays";
  EXPECT_EQ(again.dismissals, 2) << "a second tap on the same announcement, not a restart";
  EXPECT_TRUE(ShouldShowUpdateCard("v1.3.0", later, kNow + 1000)) << "greater: starts over";
}

TEST(UpdateCardTest, ALowerAnnouncedVersionNeverRestartsTheCardOrOverwritesTheState) {
  UpdateCardState state = UpdateCardAfterLater("1.2.0", {}, kNow);
  EXPECT_FALSE(ShouldShowUpdateCard("1.1.0", state, kNow + 1000)) << "rollback 1.2.0 to 1.1.0 does not re-show";
  const UpdateCardState after = UpdateCardAfterLater("1.1.0", state, kNow + 2000);
  EXPECT_EQ(after.version, "1.2.0") << "never overwritten by the lower version";
  // Re-announce 1.2.0 after the rollback: still the same announcement, hidden until its 7 days are over.
  EXPECT_FALSE(ShouldShowUpdateCard("1.2.0", state, kNow + 1000));
  EXPECT_TRUE(ShouldShowUpdateCard("1.2.0", state, kNow + kRecoveryReminderRepeatMs));
  EXPECT_TRUE(ShouldShowUpdateCard("1.2.1", state, kNow + 1000)) << "a greater one starts over";
}

TEST(UpdateCardTest, AClockThatWentBackwardsCountsAsTheSevenDaysHavingPassed) {
  const UpdateCardState state = UpdateCardAfterLater("1.2.0", {}, kNow);
  EXPECT_TRUE(ShouldShowUpdateCard("1.2.0", state, kNow - 1)) << "else it could stay hidden for good";
  UpdateCardState last = state;
  last.dismissals = 2;
  EXPECT_FALSE(ShouldShowUpdateCard("1.2.0", last, kNow - 1)) << "the second tap is still final";
}

class UpdateCardControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("pp-update-card-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    BootstrapResult boot;
    boot.profile_prefs.onboarding_completed = true;
    store_.Initialize(boot);
    chat_.BindSessionStore(store_);
    chat_.BindUpdateCard(UpdateCardPorts{
        .advice = [this]() { return advice_; },
        .machine_dir = [this]() { return dir_.string(); },
        .now_ms = [this]() { return now_ms_; },
        .open_url = [this](const std::string& url) { opened_.push_back(url); },
    });
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
  SessionStore store_;
  ChatController chat_;
  std::optional<UpdateAdvice> advice_;
  int64_t now_ms_ = kNow;
  std::vector<std::string> opened_;
};

TEST_F(UpdateCardControllerTest, ShowsOnlyWithAdviceAndOnlyAfterTheFirstRunSheets) {
  EXPECT_FALSE(chat_.UpdateCardShows());
  advice_ = UpdateAdvice{"1.2.0", "https://apps.apple.com/app/id1"};
  EXPECT_TRUE(chat_.UpdateCardShows());
  store_.Mutable().profile_prefs.onboarding_completed = false;
  EXPECT_FALSE(chat_.UpdateCardShows());
}

TEST_F(UpdateCardControllerTest, UpdateOpensTheLinkAndNothingWithoutOne) {
  advice_ = UpdateAdvice{"1.2.0", "https://apps.apple.com/app/id1"};
  chat_.OnUpdateCardOpen();
  EXPECT_EQ(opened_, std::vector<std::string>{"https://apps.apple.com/app/id1"});
  advice_ = UpdateAdvice{"1.2.0", ""};
  chat_.OnUpdateCardOpen();
  EXPECT_EQ(opened_.size(), 1u);
  EXPECT_TRUE(chat_.UpdateCardShows()) << "the card does not depend on the link";
}

// RefreshUpdateCard runs on every session-list apply: the memory is read once, not each time.
TEST_F(UpdateCardControllerTest, ARefreshDoesNotReadThePrefsFileAgain) {
  advice_ = UpdateAdvice{"1.2.0", ""};
  ASSERT_TRUE(chat_.UpdateCardShows()); // reads the file once
  // Another writer changes the file behind the controller's back: refreshes keep using what was read.
  ASSERT_TRUE(UpdatePrefs::Write(dir_.string(), UpdateCardState{"1.2.0", 2, kNow}));
  for (int i = 0; i < 3; ++i) {
    chat_.RefreshUpdateCard();
    EXPECT_TRUE(chat_.UpdateCardShows()) << "the cached state, not the file";
  }
  advice_ = UpdateAdvice{"1.3.0", ""};
  EXPECT_TRUE(chat_.UpdateCardShows());
  advice_ = UpdateAdvice{"1.2.0", ""};
  EXPECT_FALSE(chat_.UpdateCardShows()) << "the announced version changed: the file is read again";
}

TEST_F(UpdateCardControllerTest, LaterIsRememberedOnTheDeviceAndFollowsTheRhythm) {
  advice_ = UpdateAdvice{"1.2.0", ""};
  chat_.OnUpdateCardLater();
  EXPECT_FALSE(chat_.UpdateCardShows());
  EXPECT_EQ(UpdatePrefs::Read(dir_.string()).dismissals, 1) << "in machine.json, not the profile";
  now_ms_ = kNow + kRecoveryReminderRepeatMs;
  EXPECT_TRUE(chat_.UpdateCardShows());
  chat_.OnUpdateCardLater();
  now_ms_ = kNow + 90 * kRecoveryReminderRepeatMs;
  EXPECT_FALSE(chat_.UpdateCardShows());
  advice_ = UpdateAdvice{"1.3.0", ""};
  EXPECT_TRUE(chat_.UpdateCardShows());
}

} // namespace
} // namespace pbr
