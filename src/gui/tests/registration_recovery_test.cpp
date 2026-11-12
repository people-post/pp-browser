// B216: what the app does when the relay answers a poll "Relay user not registered" (RegistrationRecovery), every
// rule driven by fake signals and a fake clock.
#include "gui/chat/RegistrationRecovery.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

using State = RegistrationRecovery::State;

constexpr int64_t kT0 = 1'000'000;
constexpr int64_t kInterval = RegistrationRecovery::kRetryIntervalMs;

RecoveryActions NotRegistered(RegistrationRecovery& recovery, const int64_t now_ms, const bool auto_renew = true,
                              const bool call_live = false, const bool locked = false) {
  return recovery.OnNotRegistered(locked, auto_renew, call_live, now_ms);
}

TEST(RegistrationRecoveryTest, LockedDoesNothingAndUsesNoAttempt) {
  RegistrationRecovery recovery;
  const RecoveryActions locked = NotRegistered(recovery, kT0, /*auto_renew=*/true, false, /*locked=*/true);
  EXPECT_FALSE(locked.request_renewal);
  EXPECT_FALSE(locked.show_alert);
  EXPECT_FALSE(locked.banner_changed);
  EXPECT_EQ(recovery.CurrentState(), State::Ok);
  EXPECT_FALSE(NotRegistered(recovery, kT0, false, false, /*locked=*/true).show_alert) << "auto off, locked: nothing";

  EXPECT_TRUE(NotRegistered(recovery, kT0 + 1).request_renewal) << "unlocked right after: the attempt is still there";
}

TEST(RegistrationRecoveryTest, AutoRenewRequestsOnceAndAgainOnlyAfterTheInterval) {
  RegistrationRecovery recovery;
  const RecoveryActions first = NotRegistered(recovery, kT0);
  EXPECT_TRUE(first.request_renewal);
  EXPECT_FALSE(first.show_alert);
  EXPECT_FALSE(first.banner_changed) << "nothing shown while it recovers by itself";
  EXPECT_EQ(recovery.CurrentState(), State::AutoPending);
  EXPECT_FALSE(recovery.BannerVisible());

  EXPECT_FALSE(NotRegistered(recovery, kT0 + 2'000).request_renewal) << "the next poll, 2 s later";
  EXPECT_FALSE(NotRegistered(recovery, kT0 + kInterval - 1).request_renewal);
  EXPECT_TRUE(NotRegistered(recovery, kT0 + kInterval).request_renewal) << "5 minutes on";
  EXPECT_FALSE(NotRegistered(recovery, kT0 + kInterval + 1).request_renewal);
}

TEST(RegistrationRecoveryTest, ASuccessDoesNotResetTheInterval) {
  RegistrationRecovery recovery;
  ASSERT_TRUE(NotRegistered(recovery, kT0).request_renewal);
  EXPECT_FALSE(recovery.OnRenewal(RenewalOutcome::Renewed, true, false).request_renewal);
  recovery.OnPollOk();
  EXPECT_EQ(recovery.CurrentState(), State::Ok);

  EXPECT_FALSE(NotRegistered(recovery, kT0 + 60'000).request_renewal) << "forgotten again within 5 minutes: wait";
  EXPECT_EQ(recovery.CurrentState(), State::AutoPending);
  EXPECT_TRUE(NotRegistered(recovery, kT0 + kInterval).request_renewal);
}

TEST(RegistrationRecoveryTest, AutoRenewOffNeedsTheUserAndAlertsOncePerRun) {
  RegistrationRecovery recovery;
  const RecoveryActions first = NotRegistered(recovery, kT0, /*auto_renew=*/false);
  EXPECT_FALSE(first.request_renewal) << "never registers on its own when the user turned that off";
  EXPECT_TRUE(first.show_alert);
  EXPECT_TRUE(first.banner_changed);
  EXPECT_TRUE(first.needs_user_now);
  EXPECT_EQ(recovery.CurrentState(), State::NeedsUser);
  EXPECT_TRUE(recovery.BannerVisible());

  const RecoveryActions again = NotRegistered(recovery, kT0 + 2'000, false);
  EXPECT_FALSE(again.show_alert);
  EXPECT_FALSE(again.banner_changed);
  EXPECT_FALSE(again.needs_user_now);
  EXPECT_FALSE(again.request_renewal);

  recovery.OnPollOk();
  const RecoveryActions later = NotRegistered(recovery, kT0 + 10 * kInterval, false);
  EXPECT_TRUE(later.banner_changed) << "the row comes back";
  EXPECT_FALSE(later.show_alert) << "the alert is once per run";
}

TEST(RegistrationRecoveryTest, ARefusedRenewalNeedsTheUser) {
  RegistrationRecovery recovery;
  ASSERT_TRUE(NotRegistered(recovery, kT0).request_renewal);
  const RecoveryActions refused = recovery.OnRenewal(RenewalOutcome::Refused, true, false);
  EXPECT_TRUE(refused.banner_changed);
  EXPECT_TRUE(refused.show_alert);
  EXPECT_TRUE(refused.needs_user_now);
  EXPECT_EQ(recovery.CurrentState(), State::NeedsUser);

  const RecoveryActions next = NotRegistered(recovery, kT0 + 2'000);
  EXPECT_FALSE(next.banner_changed) << "the row stays (no flicker back to automatic)";
  EXPECT_EQ(recovery.CurrentState(), State::NeedsUser);
  EXPECT_FALSE(next.request_renewal);
  EXPECT_TRUE(NotRegistered(recovery, kT0 + kInterval).request_renewal) << "still tried every 5 minutes";
  EXPECT_TRUE(recovery.BannerVisible());
}

TEST(RegistrationRecoveryTest, ATransientFailureChangesNothing) {
  RegistrationRecovery recovery;
  ASSERT_TRUE(NotRegistered(recovery, kT0).request_renewal);
  const RecoveryActions transient = recovery.OnRenewal(RenewalOutcome::Transient, true, false);
  EXPECT_FALSE(transient.request_renewal);
  EXPECT_FALSE(transient.show_alert);
  EXPECT_FALSE(transient.banner_changed);
  EXPECT_FALSE(transient.toast_reconnected);
  EXPECT_EQ(recovery.CurrentState(), State::AutoPending);
  EXPECT_FALSE(NotRegistered(recovery, kT0 + 10'000).request_renewal) << "waits the interval";
  EXPECT_TRUE(NotRegistered(recovery, kT0 + kInterval).request_renewal);
}

TEST(RegistrationRecoveryTest, NothingDueWithAutoRenewTurnedOffMeanwhileNeedsTheUser) {
  RegistrationRecovery recovery;
  ASSERT_TRUE(NotRegistered(recovery, kT0).request_renewal);
  EXPECT_FALSE(recovery.OnRenewal(RenewalOutcome::NothingDue, /*auto_renew=*/true, false).banner_changed);
  EXPECT_EQ(recovery.CurrentState(), State::AutoPending);
  EXPECT_TRUE(recovery.OnRenewal(RenewalOutcome::NothingDue, /*auto_renew=*/false, false).banner_changed);
  EXPECT_EQ(recovery.CurrentState(), State::NeedsUser);
}

TEST(RegistrationRecoveryTest, TheAlertWaitsForTheCallToEnd) {
  RegistrationRecovery recovery;
  const RecoveryActions during = NotRegistered(recovery, kT0, false, /*call_live=*/true);
  EXPECT_FALSE(during.show_alert) << "never over a call";
  EXPECT_TRUE(during.banner_changed) << "the row is there all the same";
  EXPECT_FALSE(NotRegistered(recovery, kT0 + 2'000, false, /*call_live=*/true).show_alert);

  EXPECT_TRUE(NotRegistered(recovery, kT0 + 4'000, false, /*call_live=*/false).show_alert) << "the first signal after";
  EXPECT_FALSE(NotRegistered(recovery, kT0 + 6'000, false, false).show_alert) << "once";
}

TEST(RegistrationRecoveryTest, ARefusalDuringACallAlertsAtALaterSignal) {
  RegistrationRecovery recovery;
  ASSERT_TRUE(NotRegistered(recovery, kT0, true, /*call_live=*/true).request_renewal);
  EXPECT_FALSE(recovery.OnRenewal(RenewalOutcome::Refused, true, /*call_live=*/true).show_alert);
  EXPECT_TRUE(NotRegistered(recovery, kT0 + 2'000, true, /*call_live=*/false).show_alert);
}

TEST(RegistrationRecoveryTest, TheRowShowsOnlyWhenTheUserIsNeeded) {
  RegistrationRecovery recovery;
  EXPECT_FALSE(recovery.BannerVisible());
  NotRegistered(recovery, kT0);
  EXPECT_FALSE(recovery.BannerVisible()) << "automatic: nothing shown";
  recovery.OnRenewal(RenewalOutcome::Transient, true, false);
  EXPECT_FALSE(recovery.BannerVisible());
  recovery.OnRenewal(RenewalOutcome::Refused, true, false);
  EXPECT_TRUE(recovery.BannerVisible());
  const RecoveryActions ok = recovery.OnPollOk();
  EXPECT_TRUE(ok.banner_changed);
  EXPECT_FALSE(recovery.BannerVisible());
}

TEST(RegistrationRecoveryTest, OnlyAPollThatSucceedsReturnsToOk) {
  RegistrationRecovery recovery;
  NotRegistered(recovery, kT0, false);
  ASSERT_EQ(recovery.CurrentState(), State::NeedsUser);
  EXPECT_FALSE(recovery.OnRenewal(RenewalOutcome::Renewed, false, false).banner_changed);
  EXPECT_EQ(recovery.CurrentState(), State::NeedsUser) << "a stored registration alone is not \"connected\"";
  EXPECT_TRUE(recovery.OnUserTap());
  EXPECT_EQ(recovery.CurrentState(), State::NeedsUser) << "a tap alone is not either";

  const RecoveryActions ok = recovery.OnPollOk();
  EXPECT_EQ(recovery.CurrentState(), State::Ok);
  EXPECT_TRUE(ok.banner_changed);
  EXPECT_TRUE(ok.toast_reconnected);

  const RecoveryActions again = recovery.OnPollOk();
  EXPECT_FALSE(again.banner_changed) << "Ok already: a poll that succeeds says nothing";
  EXPECT_FALSE(again.toast_reconnected);
}

TEST(RegistrationRecoveryTest, AfterATapReconnectedIsAlwaysSaid) {
  RegistrationRecovery recovery;
  NotRegistered(recovery, kT0, false);
  ASSERT_TRUE(recovery.OnUserTap());
  EXPECT_TRUE(recovery.OnPollOk().toast_reconnected);

  NotRegistered(recovery, kT0 + 1'000, false);
  ASSERT_TRUE(recovery.OnUserTap());
  EXPECT_TRUE(recovery.OnPollOk().toast_reconnected) << "every tap that worked is answered";
}

TEST(RegistrationRecoveryTest, TheAutomaticReconnectedIsSaidOncePerRun) {
  RegistrationRecovery recovery;
  NotRegistered(recovery, kT0);
  EXPECT_TRUE(recovery.OnPollOk().toast_reconnected);
  NotRegistered(recovery, kT0 + kInterval);
  EXPECT_FALSE(recovery.OnPollOk().toast_reconnected) << "once per run";
  NotRegistered(recovery, kT0 + 2 * kInterval, false);
  ASSERT_TRUE(recovery.OnUserTap());
  EXPECT_TRUE(recovery.OnPollOk().toast_reconnected) << "a tap still gets its answer";
}

TEST(RegistrationRecoveryTest, ATapWhenAlreadyOkReconnectsNothing) {
  RegistrationRecovery recovery;
  EXPECT_FALSE(recovery.OnUserTap());
  NotRegistered(recovery, kT0, false);
  recovery.OnPollOk(); // reconnected while the alert was still up
  EXPECT_FALSE(recovery.OnUserTap());
}

TEST(RegistrationRecoveryTest, ResetSessionClearsStateAndClockButKeepsTheOncePerRunFlags) {
  RegistrationRecovery recovery;
  ASSERT_TRUE(NotRegistered(recovery, kT0).request_renewal);
  ASSERT_TRUE(recovery.OnPollOk().toast_reconnected); // the automatic toast is used
  NotRegistered(recovery, kT0 + 1'000);
  ASSERT_TRUE(recovery.OnRenewal(RenewalOutcome::Refused, true, false).show_alert); // the alert is used
  ASSERT_TRUE(recovery.OnUserTap());
  recovery.ResetSession();
  EXPECT_EQ(recovery.CurrentState(), State::Ok);
  EXPECT_FALSE(recovery.BannerVisible());

  const RecoveryActions after = NotRegistered(recovery, kT0 + 2'000);
  EXPECT_TRUE(after.request_renewal) << "the retry clock went with the session";
  EXPECT_FALSE(recovery.OnRenewal(RenewalOutcome::Refused, true, false).show_alert) << "the alert stays used";
  EXPECT_FALSE(recovery.OnPollOk().toast_reconnected) << "the tap went with the session; the automatic toast is used";
}

TEST(RegistrationRecoveryTest, RenewalResultsWhileOkAreIgnored) {
  RegistrationRecovery recovery;
  for (const RenewalOutcome outcome :
       {RenewalOutcome::Renewed, RenewalOutcome::Refused, RenewalOutcome::Transient, RenewalOutcome::NothingDue}) {
    for (const bool auto_renew : {true, false}) {
      const RecoveryActions out = recovery.OnRenewal(outcome, auto_renew, false);
      EXPECT_FALSE(out.request_renewal || out.show_alert || out.banner_changed || out.toast_reconnected);
      EXPECT_EQ(recovery.CurrentState(), State::Ok);
    }
  }
}

// The glue ChatController runs for a "not registered" poll: while locked nothing is marked or renewed; the expired mark
// is stored before the renewal runs.
TEST(RegistrationRecoveryGlueTest, LockedMarksNothingAndRequestsNoRenewal) {
  RegistrationRecovery recovery;
  std::vector<std::string> calls;
  RecoveryHooks hooks;
  hooks.mark_expired = [&] {
    calls.push_back("mark");
    return true;
  };
  hooks.run_renewal = [&] { calls.push_back("renew"); };
  hooks.show_alert = [&] { calls.push_back("alert"); };
  HandleNotRegisteredSignal(recovery, /*locked=*/true, /*auto_renew=*/true, false, kT0, hooks);
  HandleNotRegisteredSignal(recovery, /*locked=*/true, /*auto_renew=*/false, false, kT0, hooks);
  EXPECT_TRUE(calls.empty());
}

TEST(RegistrationRecoveryGlueTest, TheExpiredMarkIsStoredBeforeTheRenewalRuns) {
  RegistrationRecovery recovery;
  std::vector<std::string> calls;
  RecoveryHooks hooks;
  hooks.mark_expired = [&] {
    calls.push_back("mark");
    return true;
  };
  hooks.run_renewal = [&] { calls.push_back("renew"); };
  HandleNotRegisteredSignal(recovery, false, /*auto_renew=*/true, false, kT0, hooks);
  EXPECT_EQ(calls, (std::vector<std::string>{"mark", "renew"}));
}

TEST(RegistrationRecoveryGlueTest, NoRenewalWhenTheMarkCannotBeStored) {
  RegistrationRecovery recovery;
  std::vector<std::string> calls;
  RecoveryHooks hooks;
  hooks.mark_expired = [&] {
    calls.push_back("mark");
    return false;
  };
  hooks.run_renewal = [&] { calls.push_back("renew"); };
  HandleNotRegisteredSignal(recovery, false, true, false, kT0, hooks);
  EXPECT_EQ(calls, (std::vector<std::string>{"mark"}));
}

} // namespace
} // namespace pbr
