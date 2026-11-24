#include "feature/presenters/AccountSyncState.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace pbr {
namespace {

struct Fixture {
  std::vector<bool> saved;
  AccountSyncState state{[this](const bool pending) { saved.push_back(pending); }};
};

TEST(AccountSyncStateTest, ANewAccountIsNeverPending) {
  Fixture f;
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
  EXPECT_FALSE(f.state.OnProfileOpened(false));
  f.state.OnSyncSucceeded(); // ordinary syncs of a normal account change nothing and write nothing
  f.state.OnSyncFailed();
  f.state.OnTimeout(f.state.TimerToken());
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
  EXPECT_FALSE(f.state.Pending());
  EXPECT_TRUE(f.saved.empty());
}

TEST(AccountSyncStateTest, AnImportIsPendingUntilTheFirstSyncSucceeds) {
  Fixture f;
  EXPECT_TRUE(f.state.OnImported()); // the caller arms the timeout
  EXPECT_EQ(f.state.View(), AccountSyncView::Syncing);
  ASSERT_EQ(f.saved.size(), 1u);
  EXPECT_TRUE(f.saved.back());
  f.state.OnSyncSucceeded();
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
  ASSERT_EQ(f.saved.size(), 2u);
  EXPECT_FALSE(f.saved.back());
}

TEST(AccountSyncStateTest, AFailedSyncStopsTheSpinnerButStaysPendingUntilALaterSuccess) {
  Fixture f;
  f.state.OnImported();
  f.state.OnSyncFailed();
  EXPECT_EQ(f.state.View(), AccountSyncView::Delayed);
  EXPECT_TRUE(f.state.Pending());
  EXPECT_EQ(f.saved.size(), 1u) << "the stored flag stays set";
  f.state.OnSyncSucceeded(); // the retry got through
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
  EXPECT_FALSE(f.saved.back());
}

TEST(AccountSyncStateTest, TheTimeoutEndsTheSpinnerAndALateSuccessStillClears) {
  Fixture f;
  f.state.OnImported();
  f.state.OnTimeout(f.state.TimerToken());
  EXPECT_EQ(f.state.View(), AccountSyncView::Delayed);
  f.state.OnTimeout(f.state.TimerToken()); // a second one changes nothing
  EXPECT_EQ(f.state.View(), AccountSyncView::Delayed);
  f.state.OnSyncSucceeded();
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
}

TEST(AccountSyncStateTest, AfterTheSyncSucceededATimeoutDoesNotBringTheIndicatorBack) {
  Fixture f;
  f.state.OnImported();
  f.state.OnSyncSucceeded();
  f.state.OnTimeout(f.state.TimerToken());
  f.state.OnSyncFailed();
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
}

TEST(AccountSyncStateTest, ARelaunchWhileStillPendingShowsTheSpinnerAgain) {
  Fixture f; // a new run: the stored flag says pending
  EXPECT_TRUE(f.state.OnProfileOpened(true));
  EXPECT_EQ(f.state.View(), AccountSyncView::Syncing);
  EXPECT_TRUE(f.saved.empty()) << "reading the flag writes nothing";
  f.state.OnSyncSucceeded();
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
  ASSERT_EQ(f.saved.size(), 1u);
  EXPECT_FALSE(f.saved.back());
}

TEST(AccountSyncStateTest, OpeningTheProfileAfterAnImportDoesNotRestartTheTimeout) {
  Fixture f;
  EXPECT_TRUE(f.state.OnImported());
  EXPECT_FALSE(f.state.OnProfileOpened(true)) << "the import's timeout is already running";
  f.state.OnTimeout(f.state.TimerToken());
  EXPECT_FALSE(f.state.OnProfileOpened(true)) << "a delayed run is not made a spinner again";
  EXPECT_EQ(f.state.View(), AccountSyncView::Delayed);
}

// No recovery seed, not registered, or signed out: no sync will ever come, so the indicator goes and the flag is
// cleared — the pages show "No contacts yet" with its add button, not "Still syncing" forever.
TEST(AccountSyncStateTest, ASyncThatCanNeverRunEndsTheIndicatorAndClearsTheFlag) {
  Fixture f;
  f.state.OnImported();
  f.state.OnSyncFailed(); // "Still syncing"
  f.state.OnSyncImpossible();
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
  EXPECT_FALSE(f.state.Pending());
  ASSERT_EQ(f.saved.size(), 2u);
  EXPECT_FALSE(f.saved.back());

  Fixture spinning; // straight from the spinner too
  spinning.state.OnImported();
  spinning.state.OnSyncImpossible();
  EXPECT_EQ(spinning.state.View(), AccountSyncView::None);
  EXPECT_FALSE(spinning.saved.back());
}

TEST(AccountSyncStateTest, ASyncThatCanNeverRunOnANormalAccountWritesNothing) {
  Fixture f;
  f.state.OnSyncImpossible();
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
  EXPECT_TRUE(f.saved.empty());
}

// Reset my profile: a 60-second timer armed for the old account must not turn the new account's state "delayed".
TEST(AccountSyncStateTest, ATimerArmedBeforeAProfileResetChangesNothingAfterIt) {
  Fixture f;
  ASSERT_TRUE(f.state.OnImported());
  const uint64_t old_timer = f.state.TimerToken();
  f.state.ResetForNewProfile();
  EXPECT_EQ(f.state.View(), AccountSyncView::None);
  EXPECT_EQ(f.saved.size(), 1u) << "the reset writes nothing (the flag went with the old preferences)";

  ASSERT_TRUE(f.state.OnImported()); // the new account is linked too, within the old timer's minute
  f.state.OnTimeout(old_timer);
  EXPECT_EQ(f.state.View(), AccountSyncView::Syncing) << "the old timer is stale";
  f.state.OnTimeout(f.state.TimerToken());
  EXPECT_EQ(f.state.View(), AccountSyncView::Delayed) << "the new account's own timer still works";
}

} // namespace
} // namespace pbr
