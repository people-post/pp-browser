#include "feature/presenters/CallPresenter.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

using Outcome = CallWakePlaceholder::Outcome;

// A tapped call alert shows its page while the vault is still locked: the presenter has no backend then.
TEST(CallWakePlaceholderPresenterTest, WorksBeforeAnyBackendExistsAndEndsOnTheCallerRingLimit) {
  CallPresenter presenter;
  EXPECT_FALSE(presenter.StepWakePlaceholder(0, false, std::nullopt, 0).active) << "no tap, no page";
  EXPECT_TRUE(presenter.StepWakePlaceholder(1, true, std::nullopt, 1'000).active);
  EXPECT_TRUE(presenter.StepWakePlaceholder(1, true, std::nullopt, 60'000).active) << "locked: no ready timer";
  const auto over = presenter.StepWakePlaceholder(1, true, std::nullopt, 62'000);
  EXPECT_FALSE(over.active);
  EXPECT_EQ(over.outcome, Outcome::Expired);
  EXPECT_EQ(over.ready_ms, std::nullopt) << "messaging never became ready";
}

// "Ready" is messaging up AND a backend available; messaging up with no backend yet is not ready.
TEST(CallWakePlaceholderPresenterTest, MessagingUpWithoutABackendIsNotReady) {
  CallPresenter presenter;
  CallFunctionalPorts ports;
  ports.initialized = []() { return true; };
  presenter.BindPorts(std::move(ports));
  EXPECT_TRUE(presenter.StepWakePlaceholder(1, true, std::nullopt, 1'000).active);
  EXPECT_TRUE(presenter.StepWakePlaceholder(1, true, std::nullopt, 30'000).active) << "not ready: no 10 s timeout";
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
