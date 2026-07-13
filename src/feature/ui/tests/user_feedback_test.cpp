#include "base/error/AppError.h"

#include <gtest/gtest.h>

TEST(AppErrorTest, TypedFactoriesSetIntFields) {
  using namespace pbr;
  const Error auth = AppError::Auth(Err::Auth::Forbidden, "HTTP 403");
  EXPECT_EQ(auth.category, static_cast<int32_t>(ErrorCategory::Auth));
  EXPECT_EQ(auth.code, static_cast<int32_t>(Err::Auth::Forbidden));
  EXPECT_EQ(auth.message, "HTTP 403");
  EXPECT_TRUE(auth.user.empty());
  EXPECT_EQ(AppError::Display(auth), "Register or rotate your Brief API key in Me → Profile.");

  const Error with_user = AppError::Network(Err::Network::HttpError, "LLM HTTP 502").WithUser("Custom");
  EXPECT_EQ(AppError::Display(with_user), "Custom");
  EXPECT_NE(AppError::Log(with_user).find("user=\"Custom\""), std::string::npos);
  EXPECT_NE(AppError::Log(with_user).find("detail=\"LLM HTTP 502\""), std::string::npos);
}

TEST(AppErrorTest, PinAndConfigDisplayFromCatalog) {
  using namespace pbr;
  EXPECT_EQ(AppError::Display(AppError::Pin(Err::Pin::Required, "vault locked")),
            "Unlock your profile PIN to continue.");
  EXPECT_EQ(AppError::Display(AppError::Config(Err::Config::MissingKey, "LLM not configured")),
            "Configure the assistant in Me → Assistant.");
  EXPECT_EQ(AppError::Display(AppError::Network(Err::Network::Unreachable, "curl failed: timeout")),
            "Can't reach the server — check network or Me → Network / Assistant.");
}
