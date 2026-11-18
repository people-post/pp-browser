// Account setup failures say what happened in plain words and never show the server's English detail.
#include "foundation/i18n/LocalizationService.h"
#include "gui/onboarding/RegistrationFailureText.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(RegistrationFailureTextTest, TooManyRequestsIsTooBusy) {
  const Error limited = AppError::Network(Err::Network::HttpError, "Registration finish failed with status 429");
  EXPECT_EQ(ClassifyRegistrationFailure(limited), RegistrationFailure::TooBusy);
  EXPECT_STREQ(RegistrationFailureKey(limited), "onboarding.register_busy");
  EXPECT_EQ(ClassifyRegistrationFailure(AppError::Auth(Err::Auth::RateLimited, "x")), RegistrationFailure::TooBusy);
  EXPECT_EQ(ClassifyRegistrationFailure(AppError::Auth(Err::Auth::ServerBusy, "x")), RegistrationFailure::TooBusy);
}

TEST(RegistrationFailureTextTest, UnreachableClassErrorsSayTheServerCannotBeReached) {
  for (const Err::Network code : {Err::Network::Unreachable, Err::Network::Timeout, Err::Network::DnsFailed,
                                  Err::Network::ConnectFailed, Err::Network::TlsFailed, Err::Network::Generic}) {
    const Error error = AppError::Network(code, "x");
    EXPECT_EQ(ClassifyRegistrationFailure(error), RegistrationFailure::Unreachable) << static_cast<int>(code);
    EXPECT_STREQ(RegistrationFailureKey(error), "onboarding.register_offline");
  }
}

TEST(RegistrationFailureTextTest, EverythingElseIsTheGenericLine) {
  const Error server = AppError::Network(Err::Network::HttpError, "Registration finish failed with status 500");
  const Error refused = AppError::Network(Err::Network::HttpRejected, "Registration start failed with status 403");
  for (const Error& error : {server, refused, Error("Registration base_url not configured")}) {
    EXPECT_EQ(ClassifyRegistrationFailure(error), RegistrationFailure::Other);
    EXPECT_STREQ(RegistrationFailureKey(error), "onboarding.register_failed");
  }
}

TEST(RegistrationFailureTextTest, TheLinesArePlainChinese) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("onboarding.register_busy"), "现在开通的人有点多，请过一两分钟再试。");
  EXPECT_EQ(Tr("onboarding.register_offline"), "暂时连不上服务器，请稍后再试。");
  EXPECT_EQ(Tr("onboarding.register_failed"), "开通没成功，请稍后再试。");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
