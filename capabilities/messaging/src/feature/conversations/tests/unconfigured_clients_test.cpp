// A hub whose backend clients were never built (no backend address: the UI demo app, a blank custom backend) hands out
// a registration client that reports an error instead of dereferencing null (DEV/PLAN-ui-demo-data.md). The blob
// client is not handed out: each use takes the hub's copy and reports "not configured" when there is none.
#include "feature/conversations/ConversationsHub.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(UnconfiguredClientsTest, RegistrationReportsAnErrorInsteadOfCrashing) {
  ConversationsHub hub;
  const std::shared_ptr<IRegistrationClient> held = hub.Registration();
  ASSERT_NE(held, nullptr);
  IRegistrationClient& registration = *held;
  EXPECT_FALSE(registration.UpdateNickname("New name", "sig", 1, "account:me"));
  EXPECT_FALSE(registration.StartRegistration("pk", "name"));
  EXPECT_FALSE(registration.FinishRegistration("challenge", "pk", "name", "sig", 1));
  EXPECT_FALSE(registration.RevokeRegistration("account:me", "sig", 1));
  EXPECT_EQ(held, hub.Registration());
}

} // namespace
} // namespace pbr
