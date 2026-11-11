// DEV/PLAN-call-stranger-declined.md (b): a call "who can call me" dropped — the pill, the connecting page, the line.
#include "feature/presenters/CallPresenter.h"

#include "common/chat/RelayEnvelope.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/runtime/AppLifecycle.h"
#include "lib/calls/CallTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace pbr {
namespace {

using Outcome = CallWakePlaceholder::Outcome;
using WakeRing = CallPresenter::WakeRing;
using Clock = PendingNotificationTap::Clock;
using std::chrono::milliseconds;

BlockedCallInvite Blocked(const std::string& call_id, const std::string& caller = "account:stranger",
                          mesh::InboundAudience audience = mesh::InboundAudience::ContactsOnly) {
  BlockedCallInvite blocked;
  blocked.call_id = call_id;
  blocked.sender_identity = caller;
  blocked.audience = audience;
  return blocked;
}

class CallBlockedPresenterTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppLifecycle::SetDesktopInputFocused(true);
    CallPresenter::Callbacks callbacks;
    callbacks.notify = [this](const CallNotice& notice) {
      if (notice.kind == CallNotice::Kind::CallBlocked) {
        pills_.push_back(notice);
      }
    };
    presenter_.BindCallbacks(std::move(callbacks));
    CallFunctionalPorts ports;
    ports.record_blocked_call = [this](const BlockedCallInvite& blocked) { recorded_.push_back(blocked); };
    presenter_.BindPorts(std::move(ports));
  }
  void TearDown() override { AppLifecycle::SetDesktopInputFocused(true); }

  /** A call-alert tap (the process-wide singleton: each test records its own) with the page up for it. */
  Clock::time_point TapWithPageUp(const std::string& hint) {
    const Clock::time_point at = Clock::now();
    PendingNotificationTap::Instance().Record({}, /*call_wake=*/true, hint, at);
    EXPECT_EQ(presenter_.StepWakePlaceholder(true, at).ring, WakeRing::Show);
    return at;
  }

  CallPresenter presenter_;
  std::vector<CallNotice> pills_;
  std::vector<BlockedCallInvite> recorded_;
};

TEST_F(CallBlockedPresenterTest, ThePageWaitingForTheDroppedCallClosesWithThePill) {
  const Clock::time_point at = TapWithPageUp("call-a");
  presenter_.OnInviteBlocked(Blocked("call-a"), at + milliseconds(500));
  EXPECT_TRUE(pills_.empty()) << "the page's step says it, once";
  const auto step = presenter_.StepWakePlaceholder(true, at + milliseconds(520));
  EXPECT_EQ(step.ring, WakeRing::Clear);
  EXPECT_EQ(step.outcome, Outcome::Blocked);
  ASSERT_EQ(pills_.size(), 1u);
  EXPECT_EQ(pills_[0].key, "call.blocked.pill.contacts_only");
  EXPECT_EQ(presenter_.StepWakePlaceholder(true, at + milliseconds(540)).ring, WakeRing::Keep) << "gone for good";
  EXPECT_EQ(recorded_.size(), 1u);
}

TEST_F(CallBlockedPresenterTest, SomeoneElsesDroppedCallLeavesThePageButStillSaysSo) {
  const Clock::time_point at = TapWithPageUp("call-mine");
  presenter_.OnInviteBlocked(Blocked("call-theirs"), at + milliseconds(500));
  ASSERT_EQ(pills_.size(), 1u);
  const auto step = presenter_.StepWakePlaceholder(true, at + milliseconds(520));
  EXPECT_EQ(step.ring, WakeRing::Show);
  EXPECT_EQ(step.outcome, Outcome::None);
}

TEST_F(CallBlockedPresenterTest, ACallDroppedBeforeTheTapNeverPutsThePageUp) {
  AppLifecycle::SetDesktopInputFocused(false); // in the background when the invite came: no pill then
  const Clock::time_point blocked_at = Clock::now();
  presenter_.OnInviteBlocked(Blocked("call-early"), blocked_at);
  EXPECT_TRUE(pills_.empty());
  AppLifecycle::SetDesktopInputFocused(true);
  const Clock::time_point tapped = blocked_at + milliseconds(2'000);
  PendingNotificationTap::Instance().Record({}, /*call_wake=*/true, "call-early", tapped);
  const auto step = presenter_.StepWakePlaceholder(true, tapped);
  EXPECT_NE(step.ring, WakeRing::Show);
  EXPECT_EQ(step.outcome, Outcome::Blocked);
  EXPECT_EQ(step.shown_ms, 0);
  ASSERT_EQ(pills_.size(), 1u);
  EXPECT_NE(presenter_.StepWakePlaceholder(true, tapped + milliseconds(20)).ring, WakeRing::Show);
}

TEST_F(CallBlockedPresenterTest, TheSameCallTwiceIsOnePillAndOneRecord) {
  const Clock::time_point at = Clock::now();
  presenter_.OnInviteBlocked(Blocked("call-dup"), at);
  presenter_.OnInviteBlocked(Blocked("call-dup"), at + milliseconds(300));
  EXPECT_EQ(pills_.size(), 1u);
  EXPECT_EQ(recorded_.size(), 1u);
}

TEST_F(CallBlockedPresenterTest, TwoCallsFromOneCallerInAMinuteAreOnePill) {
  const Clock::time_point at = Clock::now();
  presenter_.OnInviteBlocked(Blocked("call-1"), at);
  presenter_.OnInviteBlocked(Blocked("call-2"), at + milliseconds(30'000));
  EXPECT_EQ(pills_.size(), 1u);
  EXPECT_EQ(recorded_.size(), 2u) << "each call is counted in the chat";
  presenter_.OnInviteBlocked(Blocked("call-3"), at + milliseconds(61'000));
  EXPECT_EQ(pills_.size(), 2u) << "a minute later it says so again";
}

TEST_F(CallBlockedPresenterTest, TheSecondCallInAMinuteClosesItsPageWithThePill) {
  const Clock::time_point at = TapWithPageUp("call-second");
  presenter_.OnInviteBlocked(Blocked("call-first"), at + milliseconds(100));
  ASSERT_EQ(pills_.size(), 1u);
  presenter_.OnInviteBlocked(Blocked("call-second"), at + milliseconds(20'000));
  const auto step = presenter_.StepWakePlaceholder(true, at + milliseconds(20'020));
  EXPECT_EQ(step.ring, WakeRing::Clear);
  EXPECT_EQ(step.outcome, Outcome::Blocked);
  EXPECT_EQ(pills_.size(), 2u) << "a page never goes without a word, whatever the minute says";
}

TEST_F(CallBlockedPresenterTest, AnAlertWithNoHintClosesThePageForAnyDroppedCall) {
  const Clock::time_point at = TapWithPageUp("");
  presenter_.OnInviteBlocked(Blocked("call-x"), at + milliseconds(500));
  const auto step = presenter_.StepWakePlaceholder(true, at + milliseconds(520));
  EXPECT_EQ(step.ring, WakeRing::Clear);
  EXPECT_EQ(step.outcome, Outcome::Blocked);
  EXPECT_EQ(pills_.size(), 1u);
}

TEST_F(CallBlockedPresenterTest, AnExpiredInviteIsOnlyRecorded) {
  const Clock::time_point at = TapWithPageUp("");
  BlockedCallInvite old = Blocked("call-old");
  old.expired = true;
  presenter_.OnInviteBlocked(old, at + milliseconds(500));
  EXPECT_TRUE(pills_.empty());
  EXPECT_EQ(presenter_.StepWakePlaceholder(true, at + milliseconds(520)).ring, WakeRing::Show) << "page left alone";
  ASSERT_EQ(recorded_.size(), 1u);
  EXPECT_TRUE(recorded_[0].expired);
}

TEST_F(CallBlockedPresenterTest, NoPillForSomeoneNotLookingButTheCallIsRecorded) {
  AppLifecycle::SetDesktopInputFocused(false);
  presenter_.OnInviteBlocked(Blocked("call-away"), Clock::now());
  EXPECT_TRUE(pills_.empty());
  EXPECT_EQ(recorded_.size(), 1u);
}

TEST_F(CallBlockedPresenterTest, AGroupsCallShowsAndRecordsNothing) {
  const Clock::time_point at = TapWithPageUp("");
  BlockedCallInvite group = Blocked("call-group");
  group.from_group = true;
  presenter_.OnInviteBlocked(group, at + milliseconds(500));
  EXPECT_TRUE(pills_.empty());
  EXPECT_TRUE(recorded_.empty());
  EXPECT_EQ(presenter_.StepWakePlaceholder(true, at + milliseconds(520)).ring, WakeRing::Show);
}

TEST_F(CallBlockedPresenterTest, NobodySaysIncomingCallsAreOff) {
  presenter_.OnInviteBlocked(Blocked("call-off", "account:contact", mesh::InboundAudience::Nobody), Clock::now());
  ASSERT_EQ(pills_.size(), 1u);
  EXPECT_EQ(pills_[0].key, "call.blocked.pill.nobody");
  EXPECT_EQ(pills_[0].identity, "account:contact");
}

// Someone the user blocked cannot make a pill, close a page or write a line by calling (texts from them are dropped).
TEST_F(CallBlockedPresenterTest, ACallerTheUserBlockedLeavesNoTrace) {
  CallFunctionalPorts ports;
  ports.record_blocked_call = [this](const BlockedCallInvite& blocked) { recorded_.push_back(blocked); };
  ports.find_contact_by_identity = [](const std::string& identity, mesh::ContactIdKind) -> Roe<std::optional<Contact>> {
    Contact contact;
    contact.local.trust = identity == "account:blocked" ? TrustLevel::Blocked : TrustLevel::Unknown;
    return std::optional<Contact>(contact);
  };
  presenter_.BindPorts(std::move(ports));
  const Clock::time_point at = TapWithPageUp("");
  presenter_.OnInviteBlocked(Blocked("call-b", "account:blocked"), at + milliseconds(500));
  EXPECT_TRUE(pills_.empty());
  EXPECT_TRUE(recorded_.empty());
  EXPECT_EQ(presenter_.StepWakePlaceholder(true, at + milliseconds(520)).ring, WakeRing::Show) << "page untouched";
  presenter_.OnInviteBlocked(Blocked("call-s", "account:stranger"), at + milliseconds(600));
  EXPECT_EQ(recorded_.size(), 1u) << "anyone else still counts";
}

TEST_F(CallBlockedPresenterTest, OurOwnAccountLeavesNoTrace) {
  CallFunctionalPorts ports;
  ports.record_blocked_call = [this](const BlockedCallInvite& blocked) { recorded_.push_back(blocked); };
  ports.local_relay_identity = []() -> std::optional<std::string> { return std::string("account:me"); };
  presenter_.BindPorts(std::move(ports));
  presenter_.OnInviteBlocked(Blocked("call-me", "account:me"), Clock::now());
  EXPECT_TRUE(pills_.empty());
  EXPECT_TRUE(recorded_.empty());
}

TEST(CallBlockedWakePlaceholderTest, TheWakeRingMetricSaysBlocked) {
  EXPECT_EQ(CallWakePlaceholder::OutcomeWord(Outcome::Blocked).view(), "blocked");
  EXPECT_EQ(CallWakePlaceholder::OutcomeWord(Outcome::Ring).view(), "ring");
  EXPECT_EQ(CallWakePlaceholder::OutcomeWord(Outcome::Timeout).view(), "timeout");
  EXPECT_EQ(CallWakePlaceholder::OutcomeWord(Outcome::Expired).view(), "expired");
}

// A ring on screen is never ended for a dropped call: the placeholder ends as Ring first.
TEST(CallBlockedWakePlaceholderTest, ARingOnScreenWinsOverADroppedCall) {
  CallWakePlaceholder placeholder;
  CallWakePlaceholder::Input in;
  in.tap_seq = 1;
  in.tap_fresh = true;
  in.now_ms = 1'000;
  EXPECT_TRUE(placeholder.Step(in).active);
  in.screen_kind = CallScreen::Kind::Ringing;
  in.blocked = true;
  in.now_ms = 1'500;
  EXPECT_EQ(placeholder.Step(in).outcome, Outcome::Ring);

  CallWakePlaceholder fresh;
  in.tap_seq = 2;
  EXPECT_EQ(fresh.Step(in).outcome, Outcome::None) << "a ring up before the page: nothing to end, nothing said";
}

// The push's hint carries only [A-Za-z0-9-]; a call id ("call:<uuid>") has a ':' — so a call alert's tap has no
// hint today, and a dropped call closes any connecting page (TapMayBeFor). If this starts to hold, the page can
// match ids instead: revisit CallPresenter::OnInviteBlocked.
TEST(CallBlockedWakePlaceholderTest, ACallIdDoesNotSurviveTheWakeHintToday) {
  EXPECT_EQ(SanitizeWakeHint(GenerateCallId()), "");
}

TEST(CallBlockedWakePlaceholderTest, ThePillWordsExistInEnglishAndChinese) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("call.blocked.pill.contacts_only"), "已设置只接好友来电，已拒接");
  EXPECT_EQ(Tr("call.blocked.pill.nobody"), "已设置不接听来电，已拒接");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("call.blocked.pill.contacts_only"), "Only contacts can call you — call declined");
  EXPECT_EQ(Tr("call.blocked.pill.nobody"), "Incoming calls are off — call declined");
}

} // namespace
} // namespace pbr
