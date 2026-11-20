// A hub whose backend clients were never built (no backend address: the UI demo app, a blank custom backend) hands out
// a registration client that reports an error instead of dereferencing null (DEV/PLAN-ui-demo-data.md). The blob
// client is not handed out: each use takes the hub's copy and reports "not configured" when there is none.
#include "feature/conversations/ConversationsHub.h"

#include "common/tests/TeardownLikeTheApp.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(UnconfiguredClientsTest, RegistrationReportsAnErrorInsteadOfCrashing) {
  test::TestOwner<ConversationsHub> hub;
  const std::shared_ptr<IRegistrationClient> held = hub->Registration();
  ASSERT_NE(held, nullptr);
  IRegistrationClient& registration = *held;
  const SignedRelayCommand signed_command{.audience = "QmRelay", .timestamp = 1, .signature = "sig"};
  EXPECT_EQ(registration.Audience(), nullptr) << "nothing is signed for no relay";
  EXPECT_FALSE(registration.UpdateNickname("New name", signed_command, "account:me"));
  EXPECT_FALSE(registration.StartRegistration("pk", "name"));
  EXPECT_FALSE(registration.FinishRegistration("challenge", "pk", "name", signed_command));
  EXPECT_FALSE(registration.RevokeRegistration("account:me", "reg-1", signed_command));
  EXPECT_EQ(held, hub->Registration());
}

} // namespace
} // namespace pbr
