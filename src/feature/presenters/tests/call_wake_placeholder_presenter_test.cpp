#include "feature/presenters/CallPresenter.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>

namespace pbr {
namespace {

using Outcome = CallWakePlaceholder::Outcome;
using WakeRing = CallPresenter::WakeRing;
using Clock = PendingNotificationTap::Clock;
using std::chrono::milliseconds;

// The tap is the process-wide singleton: every test records its own (a new sequence number) at a time of its own.
Clock::time_point Tapped(const std::optional<milliseconds> age = std::nullopt) {
  const Clock::time_point at = Clock::now();
  PendingNotificationTap::Instance().Record({}, /*call_wake=*/true, {}, at, age);
  return at;
}

// A tapped call alert shows its page while the vault is still locked: the presenter has no backend then.
TEST(CallWakePlaceholderPresenterTest, ShowsBeforeAnyBackendExistsAndEndsOnTheCallerRingLimit) {
  CallPresenter presenter;
  const Clock::time_point at = Tapped();
  EXPECT_EQ(presenter.StepWakePlaceholder(true, at).ring, WakeRing::Show);
  EXPECT_EQ(presenter.StepWakePlaceholder(true, at + milliseconds(59'000)).ring, WakeRing::Show) << "locked: no timer";
  const auto over = presenter.StepWakePlaceholder(true, at + milliseconds(61'000));
  EXPECT_EQ(over.ring, WakeRing::Clear);
  EXPECT_EQ(over.outcome, Outcome::Expired);
  EXPECT_EQ(over.ready_ms, std::nullopt) << "messaging never became ready";
  EXPECT_EQ(presenter.StepWakePlaceholder(true, at + milliseconds(62'000)).ring, WakeRing::Keep) << "cleared once";
}

// "Ready" is messaging up AND a backend available; messaging up with no backend yet is not ready.
TEST(CallWakePlaceholderPresenterTest, MessagingUpWithoutABackendIsNotReady) {
  CallPresenter presenter;
  CallFunctionalPorts ports;
  ports.initialized = []() { return true; };
  presenter.BindPorts(std::move(ports));
  const Clock::time_point at = Tapped();
  EXPECT_EQ(presenter.StepWakePlaceholder(true, at).ring, WakeRing::Show);
  EXPECT_EQ(presenter.StepWakePlaceholder(true, at + milliseconds(30'000)).ring, WakeRing::Show)
      << "not ready: no 10 s timeout";
}

TEST(CallWakePlaceholderPresenterTest, TheCapCountsFromTheTapNotFromTheFirstStep) {
  CallPresenter presenter;
  const Clock::time_point at = Tapped(milliseconds(59'000));
  EXPECT_EQ(presenter.StepWakePlaceholder(true, at + milliseconds(2'500)).ring, WakeRing::Keep)
      << "a 59 s alert tapped 2.5 s ago: the caller has hung up";
}

TEST(CallWakePlaceholderPresenterTest, NoTapNoPageAndAnOldTapIsNotATap) {
  Tapped();
  CallPresenter fresh;
  // 91 s later the tap's route (90 s) is gone: nothing to show.
  EXPECT_EQ(fresh.StepWakePlaceholder(true, Clock::now() + milliseconds(91'000)).ring, WakeRing::Keep);
}

TEST(CallWakePlaceholderPresenterTest, AnythingButAPhoneNeverShowsAndClearsOneThatWasUp) {
  CallPresenter presenter;
  const Clock::time_point at = Tapped();
  EXPECT_EQ(presenter.StepWakePlaceholder(false, at).ring, WakeRing::Keep) << "desktop: no page";
  EXPECT_EQ(presenter.StepWakePlaceholder(true, at).ring, WakeRing::Show);
  EXPECT_EQ(presenter.StepWakePlaceholder(false, at).ring, WakeRing::Clear) << "the layout left the phone's";
  EXPECT_EQ(presenter.StepWakePlaceholder(false, at).ring, WakeRing::Keep);
}

TEST(CallWakePlaceholderPresenterTest, TheTitleWordExistsInEnglishAndChinese) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  const std::string en = Tr("call.ring.awaiting.title");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  const std::string zh = Tr("call.ring.awaiting.title");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(en, "Incoming call, connecting…");
  EXPECT_EQ(zh, "来电，正在接入…");
}

} // namespace
} // namespace pbr
