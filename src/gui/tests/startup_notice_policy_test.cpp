#include "gui/chat/StartupNoticePolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

Error Net(Err::Network code) {
  return AppError::Network(code, "test");
}

TEST(StartupNoticePolicyTest, UnreachableClassIsDnsRouteTimeoutAndIdentityFetch) {
  for (Err::Network code :
       {Err::Network::Generic, Err::Network::Unreachable, Err::Network::Timeout, Err::Network::DnsFailed,
        Err::Network::ConnectFailed, Err::Network::TlsFailed, Err::Network::RelayIdentityUnavailable}) {
    EXPECT_TRUE(IsNetworkUnreachable(Net(code))) << static_cast<int>(code);
  }
  for (Err::Network code : {Err::Network::HttpError, Err::Network::HttpRejected, Err::Network::RecipientNotRegistered,
                            Err::Network::PaymentRequired}) {
    EXPECT_FALSE(IsNetworkUnreachable(Net(code))) << static_cast<int>(code);
  }
  EXPECT_FALSE(IsNetworkUnreachable(AppError::Auth(Err::Auth::Forbidden, "x")));
  EXPECT_FALSE(IsNetworkUnreachable(AppError::Internal("x")));
}

TEST(StartupNoticePolicyTest, BackgroundUnreachableIsQuietUntilFirstSuccessOrWindowEnd) {
  const Error dns = Net(Err::Network::DnsFailed);
  EXPECT_FALSE(ShowBackgroundFailure(dns, false, 0, false));
  EXPECT_FALSE(ShowBackgroundFailure(dns, false, kStartupQuietWindowMs - 1, false));
  EXPECT_TRUE(ShowBackgroundFailure(dns, false, kStartupQuietWindowMs, false));
  // After something worked, a later outage is real news.
  EXPECT_TRUE(ShowBackgroundFailure(dns, false, 1000, true));
}

TEST(StartupNoticePolicyTest, WhatTheUserJustDidAlwaysShows) {
  EXPECT_TRUE(ShowBackgroundFailure(Net(Err::Network::DnsFailed), true, 0, false));
  EXPECT_TRUE(ShowBackgroundFailure(Net(Err::Network::Timeout), true, 100, false));
}

TEST(StartupNoticePolicyTest, AServerAnswerOrAnotherKindOfErrorIsNeverHidden) {
  EXPECT_TRUE(ShowBackgroundFailure(Net(Err::Network::HttpError), false, 0, false));
  EXPECT_TRUE(ShowBackgroundFailure(Net(Err::Network::HttpRejected), false, 0, false));
  EXPECT_TRUE(ShowBackgroundFailure(AppError::Auth(Err::Auth::Forbidden, "x"), false, 0, false));
}

} // namespace
} // namespace pbr
