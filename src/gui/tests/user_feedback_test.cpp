#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace {

class AppErrorTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    // The expected strings are English; a Chinese OS would otherwise pick zh-Hans.
    pbr::LocalizationService::Instance().SetPreferredLanguage("en");
  }
};

} // namespace

TEST_F(AppErrorTest, TypedFactoriesSetIntFields) {
  using namespace pbr;
  const Error auth = AppError::Auth(Err::Auth::Forbidden, "HTTP 403");
  EXPECT_EQ(auth.category, static_cast<int32_t>(ErrorCategory::Auth));
  EXPECT_EQ(auth.code, static_cast<int32_t>(Err::Auth::Forbidden));
  EXPECT_EQ(auth.message, "HTTP 403");
  EXPECT_TRUE(auth.user.empty());
  EXPECT_EQ(AppError::Display(auth), "Set up your account in Me → Profile.");

  const Error with_user = AppError::Network(Err::Network::HttpError, "LLM HTTP 502").WithUser("Custom");
  EXPECT_EQ(AppError::Display(with_user), "Custom");
  EXPECT_NE(AppError::Log(with_user).find("user=\"Custom\""), std::string::npos);
  EXPECT_NE(AppError::Log(with_user).find("detail=\"LLM HTTP 502\""), std::string::npos);
}

TEST_F(AppErrorTest, PinAndConfigDisplayFromCatalog) {
  using namespace pbr;
  EXPECT_EQ(AppError::Display(AppError::Pin(Err::Pin::Required, "vault locked")),
            "Unlock your profile PIN to continue.");
  EXPECT_EQ(AppError::Display(AppError::Config(Err::Config::MissingKey, "LLM not configured")),
            "Configure the assistant in Me → Assistant.");
  EXPECT_EQ(AppError::Display(AppError::Network(Err::Network::Unreachable, "curl failed: recv error")),
            "Can't reach the server.");
}

TEST_F(AppErrorTest, NetworkDisplayNamesTheCause) {
  using namespace pbr;
  EXPECT_EQ(AppError::Display(AppError::Network(Err::Network::DnsFailed, "curl failed: resolve")),
            "Can't find the server's address — the network may not be connected.");
  EXPECT_EQ(AppError::Display(AppError::Network(Err::Network::ConnectFailed, "curl failed: connect")),
            "Can't connect to the server — it may be down, or a firewall is blocking it.");
  EXPECT_EQ(AppError::Display(AppError::Network(Err::Network::Timeout, "curl failed: timeout")),
            "The server took too long to respond — the network is slow or the server isn't answering.");
  EXPECT_EQ(AppError::Display(AppError::Network(Err::Network::TlsFailed, "curl failed: ssl")),
            "Can't set up a secure connection — the network may be going through a proxy.");
  EXPECT_EQ(AppError::Display(AppError::Blob(Err::Blob::UploadNotConfigured, "Blob upload is not configured")),
            "Can't send pictures right now — the server hasn't enabled uploads.");
}
