// The hub's registration in three steps (DEV/PLAN-register-async.md): Begin and Finish on the UI thread, Run on a worker.
// A hub that was never initialized is refused at the first and last step, and writes nothing; RegisterIdentity (their
// composition: onboarding, app_probe) answers the same. The ready path (nickname saved first, addresses and card read)
// needs a running hub: RegistrationJobTest (domain/registration) covers what the job sends and stores, a device the rest.
#include "feature/conversations/ConversationsHub.h"

#include "foundation/error/AppError.h"

#include "common/tests/TeardownLikeTheApp.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

void ExpectPinRequired(const Error& error) {
  EXPECT_EQ(AppError::CategoryOf(error), ErrorCategory::Pin);
  EXPECT_EQ(error.code, static_cast<int32_t>(Err::Pin::Required));
}

TEST(RegistrationSplitTest, ABeginOnAHubThatWasNeverInitializedIsRefused) {
  test::TestOwner<ConversationsHub> hub;
  auto job = hub->BeginRegistration("Nick");
  ASSERT_FALSE(static_cast<bool>(job));
  ExpectPinRequired(job.error());
  auto without_nickname = hub->BeginRegistration("");
  ASSERT_FALSE(static_cast<bool>(without_nickname));
  ExpectPinRequired(without_nickname.error());
}

TEST(RegistrationSplitTest, AFinishOnAHubThatWasNeverInitializedIsRefusedAndWritesNothing) {
  test::TestOwner<ConversationsHub> hub;
  RegistrationResult result;
  result.success = true;
  result.account_id = "account:a";
  auto finished = hub->FinishRegistration(result, "account:a");
  ASSERT_FALSE(static_cast<bool>(finished));
  ExpectPinRequired(finished.error());
}

TEST(RegistrationSplitTest, RegisterIdentityAnswersTheSameErrorAsBefore) {
  test::TestOwner<ConversationsHub> hub;
  auto registered = hub->RegisterIdentity("Nick");
  ASSERT_FALSE(static_cast<bool>(registered));
  ExpectPinRequired(registered.error());
}

TEST(RegistrationSplitTest, ARunWithoutAClientIsAnErrorNotACrash) {
  RegistrationJob job;
  EXPECT_FALSE(static_cast<bool>(ConversationsHub::RunRegistration(job)));
}

} // namespace
} // namespace pbr
