#include "lib/net/RelayAudience.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

// NoteRelayRefusal (SERVICE_ENDPOINTS § Client compatibility): who must update is typed apart (U11), the card codes at
// registration are typed (X12), and the refusals asking again will not change are told apart (T14).

std::string Coded(const std::string& code) {
  return R"({"error":"x","code":")" + code + R"("})";
}

TEST(RelayAudienceRefusalTest, WhoMustUpdateIsTypedApart) {
  std::vector<std::string> heard;
  FixedRelayAudience audience("QmPeer", [&heard](const std::string& code) { heard.push_back(code); });

  const auto app_too_old = NoteRelayRefusal(&audience, 426, Coded("client_upgrade_required"));
  ASSERT_TRUE(app_too_old);
  EXPECT_TRUE(IsClientUpgradeRequired(*app_too_old));
  EXPECT_FALSE(IsRelayUpgradeRequired(*app_too_old));

  for (const char* code : {"relay_upgrade_required", "unsupported_sign_version"}) {
    const auto relay_too_old = NoteRelayRefusal(&audience, 400, Coded(code));
    ASSERT_TRUE(relay_too_old) << code;
    EXPECT_TRUE(IsRelayUpgradeRequired(*relay_too_old)) << code;
    EXPECT_FALSE(IsClientUpgradeRequired(*relay_too_old)) << code;
  }
  EXPECT_EQ(heard,
            (std::vector<std::string>{"client_upgrade_required", "relay_upgrade_required", "relay_upgrade_required"}));
  EXPECT_FALSE(NoteRelayRefusal(&audience, 403, Coded("relay_upgrade_required"))) << "only as a 400";
}

TEST(RelayAudienceRefusalTest, TheCardCodesAtRegistrationAreTyped) {
  for (const char* code : kAccountCardRefusalCodes) {
    const auto refused = NoteRelayRefusal(nullptr, 400, Coded(code));
    ASSERT_TRUE(refused) << code;
    EXPECT_EQ(AppError::CategoryOf(*refused), ErrorCategory::Auth);
    EXPECT_EQ(static_cast<Err::Auth>(refused->code), Err::Auth::AccountCardRefused) << code;
    EXPECT_NE(refused->message.find(code), std::string::npos) << "the detail names the code";
    EXPECT_FALSE(IsPermanentRelayRefusal(*refused)) << "a new card may be taken";
  }
  EXPECT_FALSE(NoteRelayRefusal(nullptr, 400, Coded("invalid_request"))) << "another 400 is worded as before";
}

TEST(RelayAudienceRefusalTest, PermanentRefusals) {
  EXPECT_TRUE(IsPermanentRelayRefusal(AppError::Auth(Err::Auth::ClientUpgradeRequired, "x")));
  EXPECT_TRUE(IsPermanentRelayRefusal(AppError::Auth(Err::Auth::RelayUpgradeRequired, "x")));
  EXPECT_TRUE(IsPermanentRelayRefusal(AppError::Auth(Err::Auth::WrongAudience, "x")));
  EXPECT_FALSE(IsPermanentRelayRefusal(AppError::Auth(Err::Auth::NotRegistered, "x")));
  EXPECT_FALSE(IsPermanentRelayRefusal(AppError::Network(Err::Network::ConnectFailed, "x")));
  EXPECT_FALSE(IsPermanentRelayRefusal(Error("plain")));
}

} // namespace
} // namespace pbr
