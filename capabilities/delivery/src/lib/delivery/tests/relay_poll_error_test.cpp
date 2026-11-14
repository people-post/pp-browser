// B216: a relay poll answered 403 `not_registered` is told apart from every other failure, so the app can register
// again; every other status (and a transport failure) keeps its old type.
#include "lib/delivery/HttpRelayClient.h"
#include "lib/delivery/RelayClients.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr const char* kNotRegisteredBody = R"({"error":"Account not registered","code":"not_registered"})";

TEST(RelayPollErrorTest, A403WithTheNotRegisteredCodeIsNotRegistered) {
  const Error error = RelayPollHttpError(403, kNotRegisteredBody);
  EXPECT_TRUE(IsRelayUserNotRegistered(error));
  EXPECT_EQ(AppError::CategoryOf(error), ErrorCategory::Auth);
  EXPECT_NE(error.message.find("Account not registered"), std::string::npos) << "the log keeps the server's words";
}

TEST(RelayPollErrorTest, Another403BodyIsARejectionAsBefore) {
  for (const char* body :
       {R"({"error":"Registration expired","code":"registration_expired"})", R"({"error":"Relay user not registered"})",
        R"({"code":"NOT_REGISTERED"})", "not_registered", ""}) {
    const Error error = RelayPollHttpError(403, body);
    EXPECT_FALSE(IsRelayUserNotRegistered(error)) << body;
    EXPECT_EQ(AppError::CategoryOf(error), ErrorCategory::Network) << body;
    EXPECT_EQ(error.code, static_cast<int32_t>(Err::Network::HttpRejected)) << body;
  }
}

TEST(RelayPollErrorTest, OtherStatusesAreNotNotRegistered) {
  for (const long status : {401L, 404L, 429L, 500L}) {
    const Error error = RelayPollHttpError(status, kNotRegisteredBody);
    EXPECT_FALSE(IsRelayUserNotRegistered(error)) << status;
    EXPECT_EQ(AppError::CategoryOf(error), ErrorCategory::Network) << status;
  }
  EXPECT_EQ(RelayPollHttpError(401, kNotRegisteredBody).code, static_cast<int32_t>(Err::Network::HttpRejected));
  EXPECT_EQ(RelayPollHttpError(429, kNotRegisteredBody).code, static_cast<int32_t>(Err::Network::HttpError));
  EXPECT_EQ(RelayPollHttpError(500, kNotRegisteredBody).code, static_cast<int32_t>(Err::Network::HttpError));
}

TEST(RelayPollErrorTest, ATransportFailureIsNotNotRegistered) {
  EXPECT_FALSE(IsRelayUserNotRegistered(AppError::Network(Err::Network::Timeout, "timed out")));
  EXPECT_FALSE(IsRelayUserNotRegistered(AppError::Network(Err::Network::ConnectFailed, "connect failed")));
  EXPECT_FALSE(IsRelayUserNotRegistered(Error("not_registered"))) << "an untyped error with the same words";
}

} // namespace
} // namespace pbr
