#include "feature/presenters/CallConnectStory.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// The call page's connection story says only what really happened, in plain words, and changes rarely (B146).

using Status = CallScreen::Status;
using Kind = CallScreen::Kind;

CallStoryInput At(int64_t now_ms, Status status, bool live = false, const char* path = "direct",
                  CallPathQuality quality = CallPathQuality::Good, int bars = 4) {
  CallStoryInput in;
  in.call_id = "call-1";
  in.kind = Kind::InCall;
  in.placed_here = true;
  in.status = status;
  in.media_connected = live;
  in.path_kind = path;
  in.quality = quality;
  in.quality_bars = bars;
  in.now_ms = now_ms;
  return in;
}

TEST(CallConnectStoryTest, AnOutgoingCallShowsCallingThenAnsweredThenTheResultWithItsSeconds) {
  CallConnectStory story;
  CallStoryView v = story.Step(At(1'000, Status::Calling));
  EXPECT_TRUE(v.connecting);
  EXPECT_TRUE(v.outgoing);
  EXPECT_EQ(v.stage, 0);
  EXPECT_FALSE(v.signal_shown);

  v = story.Step(At(4'000, Status::Connecting, false, ""));
  EXPECT_TRUE(v.connecting);
  EXPECT_EQ(v.stage, 1) << "leaving Calling means the other side answered";

  // Connected 1.8 s after the answer, around the network limits.
  v = story.Step(At(5'800, Status::Connected, true, "punched"));
  EXPECT_FALSE(v.connecting);
  EXPECT_TRUE(v.result_shown);
  EXPECT_FALSE(v.result_fading);
  EXPECT_EQ(v.result_route, CallRoute::AroundLimits);
  EXPECT_EQ(v.connect_tenths, 18) << "from the answer, not from placing the call (ringing is not connecting)";
  EXPECT_FALSE(v.signal_shown) << "the result line comes first";

  ASSERT_EQ(v.events.size(), 4u);
  EXPECT_EQ(v.events[0].kind, CallStoryEvent::Kind::Placed);
  EXPECT_EQ(v.events[1].kind, CallStoryEvent::Kind::PeerAnswered);
  EXPECT_EQ(v.events[1].at_ms, 3'000);
  EXPECT_EQ(v.events[2].kind, CallStoryEvent::Kind::Connected);
  EXPECT_EQ(v.events[2].route, CallRoute::AroundLimits);
  EXPECT_EQ(v.events[3].kind, CallStoryEvent::Kind::Encrypted);
}

// #11: a "calling" answered within 300 ms is still shown 600 ms so it is seen to pass; a slower answer moves on at
// once (no made-up delays); connecting ends the hold.
TEST(CallConnectStoryTest, AQuickAnswerKeepsCallingOnScreenForAMoment) {
  CallConnectStory story;
  EXPECT_EQ(story.Step(At(1'000, Status::Calling)).stage, 0);
  CallStoryView v = story.Step(At(1'200, Status::Connecting, false, ""));
  EXPECT_EQ(v.stage, 0) << "answered after 200 ms: calling stays";
  ASSERT_EQ(v.events.size(), 2u) << "the history has the answer at once";
  EXPECT_EQ(v.events[1].kind, CallStoryEvent::Kind::PeerAnswered);
  EXPECT_EQ(story.Step(At(1'599, Status::Connecting, false, "")).stage, 0);
  EXPECT_EQ(story.Step(At(1'600, Status::Connecting, false, "")).stage, 1);

  CallConnectStory slow;
  slow.Step(At(1'000, Status::Calling));
  EXPECT_EQ(slow.Step(At(1'300, Status::Connecting, false, "")).stage, 1) << "300 ms is not quick: no hold";

  CallConnectStory live;
  live.Step(At(1'000, Status::Calling));
  live.Step(At(1'100, Status::Connecting, false, ""));
  v = live.Step(At(1'200, Status::Connected, true, "direct"));
  EXPECT_FALSE(v.connecting);
  EXPECT_EQ(v.stage, 1) << "connected: the hold is over";
}

TEST(CallConnectStoryTest, TheResultLineFadesAfterFourSecondsThenTheSignalBarShows) {
  CallConnectStory story;
  story.Step(At(0, Status::Calling));
  story.Step(At(1'000, Status::Connecting, false, ""));
  story.Step(At(2'000, Status::Connected, true, "direct"));
  CallStoryView v = story.Step(At(2'000 + 3'999, Status::Connected, true, "direct"));
  EXPECT_TRUE(v.result_shown);
  EXPECT_FALSE(v.result_fading);
  v = story.Step(At(2'000 + 4'000, Status::Connected, true, "direct"));
  EXPECT_TRUE(v.result_shown);
  EXPECT_TRUE(v.result_fading);
  EXPECT_FALSE(v.signal_shown);
  v = story.Step(At(2'000 + 4'400, Status::Connected, true, "direct"));
  EXPECT_FALSE(v.result_shown);
  EXPECT_TRUE(v.signal_shown);
  EXPECT_EQ(v.signal_tone, CallSignalTone::Good);
}

TEST(CallConnectStoryTest, AnAnsweredCallStartsAtTheSecondStepAndCountsFromTheAccept) {
  CallConnectStory story;
  CallStoryInput in = At(10'000, Status::Connecting, false, "");
  in.kind = Kind::Answering;
  CallStoryView v = story.Step(in);
  EXPECT_FALSE(v.outgoing);
  EXPECT_EQ(v.stage, 1);
  ASSERT_EQ(v.events.size(), 1u);
  EXPECT_EQ(v.events[0].kind, CallStoryEvent::Kind::Answered);
  v = story.Step(At(10'900, Status::Connected, true, "circuit"));
  EXPECT_EQ(v.result_route, CallRoute::Relayed);
  EXPECT_EQ(v.connect_tenths, 9);
}

TEST(CallConnectStoryTest, ARingThatFailsIsNotToldAsAnswered) {
  CallConnectStory story;
  story.Step(At(0, Status::Calling));
  const CallStoryView v = story.Step(At(30'000, Status::CouldntConnect));
  EXPECT_FALSE(v.connecting);
  EXPECT_EQ(v.events.size(), 1u) << "only Placed";
  EXPECT_FALSE(v.signal_shown);
}

TEST(CallConnectStoryTest, AGroupCallHasNoResultLineAndGoesStraightToTheBar) {
  CallConnectStory story;
  CallStoryInput in = At(0, Status::SettingUpGroup, false, "");
  in.group = true;
  in.placed_here = false;
  CallStoryView v = story.Step(in);
  EXPECT_TRUE(v.connecting);
  EXPECT_EQ(v.stage, 1) << "joined, the group's line is being set up";
  in = At(2'000, Status::Connected, true, "media_relay");
  in.group = true;
  in.placed_here = false;
  v = story.Step(in);
  EXPECT_FALSE(v.result_shown);
  EXPECT_TRUE(v.signal_shown);
}

TEST(CallConnectStoryTest, AReconnectRaisesNoTopPillTheCapsuleAboveTheButtonsOwnsIt) {
  CallConnectStory story;
  story.Step(At(0, Status::Connected, true, "direct"));
  const CallStoryView v =
      story.Step(At(10'000, Status::Reconnecting, false, "direct", CallPathQuality::Reconnecting, 0));
  EXPECT_EQ(v.notice, CallStoryNotice::None);
}

TEST(CallConnectStoryTest, ARelayedCallThatMovesCloserSaysSoAndLogsItInTheTimeline) {
  CallConnectStory story;
  story.Step(At(0, Status::Connected, true, "circuit"));
  CallStoryView v = story.Step(At(20'000, Status::Connected, true, "circuit"));
  EXPECT_EQ(v.notice, CallStoryNotice::None);
  v = story.Step(At(21'000, Status::Connected, true, "punched"));
  EXPECT_EQ(v.notice, CallStoryNotice::Closer);
  EXPECT_EQ(v.route, CallRoute::AroundLimits);
  ASSERT_FALSE(v.events.empty());
  EXPECT_EQ(v.events.back().kind, CallStoryEvent::Kind::Closer);
  EXPECT_EQ(v.events.back().call_ms, 21'000);
  // A direct call going relayed is no "closer" news.
  CallConnectStory other;
  other.Step(At(0, Status::Connected, true, "direct"));
  v = other.Step(At(5'000, Status::Connected, true, "circuit"));
  EXPECT_EQ(v.notice, CallStoryNotice::None);
}

TEST(CallConnectStoryTest, TheSignalBarChangesAtMostEveryHalfSecond) {
  CallConnectStory story;
  story.Step(At(0, Status::Connected, true, "direct", CallPathQuality::Good, 4));
  CallStoryView v = story.Step(At(100, Status::Connected, true, "direct", CallPathQuality::Good, 3));
  EXPECT_EQ(v.signal_bars, 4) << "held";
  v = story.Step(At(499, Status::Connected, true, "direct", CallPathQuality::Good, 3));
  EXPECT_EQ(v.signal_bars, 4);
  v = story.Step(At(500, Status::Connected, true, "direct", CallPathQuality::Good, 3));
  EXPECT_EQ(v.signal_bars, 3);
  v = story.Step(At(600, Status::Connected, true, "direct", CallPathQuality::Good, 2));
  EXPECT_EQ(v.signal_bars, 3);
}

TEST(CallConnectStoryTest, TheLineGotSlowerIsSaidOnGoodTurningWorseAtMostEveryThirtySeconds) {
  CallConnectStory story;
  story.Step(At(0, Status::Connected, true, "direct", CallPathQuality::Good, 4));
  CallStoryView v = story.Step(At(1'000, Status::Connected, true, "direct", CallPathQuality::Fair, 2));
  EXPECT_EQ(v.notice, CallStoryNotice::Slow);
  EXPECT_EQ(v.signal_tone, CallSignalTone::Fair);
  v = story.Step(At(2'000, Status::Connected, true, "direct", CallPathQuality::Poor, 1));
  EXPECT_EQ(v.signal_tone, CallSignalTone::Poor);
  // Back to good and worse again within 30 s: the bar says it, the pill does not.
  story.Step(At(5'000, Status::Connected, true, "direct", CallPathQuality::Good, 4));
  v = story.Step(At(6'000, Status::Connected, true, "direct", CallPathQuality::Fair, 2));
  EXPECT_EQ(v.notice, CallStoryNotice::None);
  story.Step(At(40'000, Status::Connected, true, "direct", CallPathQuality::Good, 4));
  v = story.Step(At(41'000, Status::Connected, true, "direct", CallPathQuality::Fair, 2));
  EXPECT_EQ(v.notice, CallStoryNotice::Slow);
}

TEST(CallConnectStoryTest, ANewCallStartsANewStory) {
  CallConnectStory story;
  story.Step(At(0, Status::Connected, true, "circuit"));
  CallStoryInput in = At(50'000, Status::Calling);
  in.call_id = "call-2";
  const CallStoryView v = story.Step(in);
  EXPECT_TRUE(v.connecting);
  EXPECT_EQ(v.route, CallRoute::Unknown);
  EXPECT_EQ(v.events.size(), 1u);
}

TEST(CallConnectStoryTest, PathKindsMapToPlainRoutes) {
  EXPECT_EQ(CallRouteFromPathKind("direct"), CallRoute::Direct);
  EXPECT_EQ(CallRouteFromPathKind("punched"), CallRoute::AroundLimits);
  EXPECT_EQ(CallRouteFromPathKind("circuit"), CallRoute::Relayed);
  EXPECT_EQ(CallRouteFromPathKind("media_relay"), CallRoute::Relayed);
  EXPECT_EQ(CallRouteFromPathKind(""), CallRoute::Unknown);
  EXPECT_STREQ(CallSignalWordKey(CallPathQuality::Good), "");
  EXPECT_STREQ(CallSignalWordKey(CallPathQuality::Fair), "call.quality.fair");
  EXPECT_STREQ(CallSignalWordKey(CallPathQuality::Poor), "call.quality.poor");
}

TEST(CallConnectStoryTest, EveryWordExistsInEnglishAndChineseAndNoneIsATechWord) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  std::vector<std::string> keys;
  for (const CallRoute route : {CallRoute::Unknown, CallRoute::Direct, CallRoute::AroundLimits, CallRoute::Relayed}) {
    keys.emplace_back(CallRouteResultKey(route));
    keys.emplace_back(CallRouteHowKey(route));
    keys.emplace_back(CallRouteEventKey(route));
  }
  for (const CallStoryNotice notice : {CallStoryNotice::Slow, CallStoryNotice::Closer}) {
    keys.emplace_back(CallStoryNoticeKey(notice));
  }
  for (const char* tip : kCallStoryTipKeys) {
    keys.emplace_back(tip);
  }
  for (const char* key : {"call.reconnect.restored", "call.reconnect.offline", "call.status.reconnecting",
                          "call.quality.hint.connecting_voice", "call.quality.hint.connecting_video",
                          "call.quality.hint.retrying_voice", "call.quality.hint.retrying_video",
                          "call.story.result", "call.story.step.calling", "call.story.step.peer_answered",
                          "call.story.step.you_answered", "call.story.step.securing", "call.story.step.connected",
                          "call.story.step.group_joined", "call.story.step.group_calling", "call.story.step.group_line",
                          "call.story.step.group_heard", "call.story.sheet.title", "call.story.sheet.lead",
                          "call.story.sheet.now", "call.story.sheet.how", "call.story.sheet.encryption",
                          "call.story.sheet.e2e", "call.story.event.placed", "call.story.event.answered",
                          "call.story.event.joined", "call.story.event.peer_answered", "call.story.event.encrypted",
                          "call.story.event.group_encrypted", "call.story.event.group_connected",
                          "call.story.event.closer", "call.story.seconds", "call.story.prefix"}) {
    keys.emplace_back(key);
  }
  for (const std::string& key : keys) {
    LocalizationService::Instance().SetPreferredLanguage("en");
    const std::string en = Tr(key);
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    const std::string zh = Tr(key);
    EXPECT_FALSE(en.empty()) << key;
    EXPECT_NE(en, key) << key;
    EXPECT_NE(zh, key) << key;
    for (const char* tech : {"打洞", "中继", "NAT", "relay", "punch", " ms"}) {
      EXPECT_EQ(zh.find(tech), std::string::npos) << key << " says " << tech;
      EXPECT_EQ(en.find(tech), std::string::npos) << key << " says " << tech;
    }
  }
}

} // namespace
} // namespace pbr

namespace pbr {
namespace {

// A joiner from a group's join bar first reads "Calling" too (its media is starting): it joined, it is not calling.
TEST(CallConnectStoryTest, AJoinerIsJoiningNotCallingEveryone) {
  CallConnectStory story;
  CallStoryInput in = At(1'000, Status::Calling);
  in.group = true;
  in.placed_here = false;
  const CallStoryView v = story.Step(in);
  EXPECT_FALSE(v.outgoing);
  EXPECT_EQ(v.stage, 1);
  ASSERT_EQ(v.events.size(), 1u);
  EXPECT_EQ(v.events[0].kind, CallStoryEvent::Kind::Answered);
  EXPECT_EQ(v.events[0].wall_ms, 1'000);
}

// Audio warms up after media connects: no bar, no "slower" until the first good sample (or 2 s).
TEST(CallConnectStoryTest, TheBarWaitsForAudioToSettleAfterConnect) {
  CallConnectStory story;
  CallStoryInput in = At(0, Status::Connected, true, "direct", CallPathQuality::NoAudio, 0);
  in.group = true;
  CallStoryView v = story.Step(in);
  EXPECT_FALSE(v.signal_shown);
  in = At(800, Status::Connected, true, "direct", CallPathQuality::Good, 4);
  in.group = true;
  v = story.Step(in);
  EXPECT_TRUE(v.signal_shown);
  EXPECT_EQ(v.signal_tone, CallSignalTone::Good);
  EXPECT_EQ(v.notice, CallStoryNotice::None) << "warming up is not a slower line";
  in = At(1'400, Status::Connected, true, "direct", CallPathQuality::Fair, 2);
  in.group = true;
  v = story.Step(in);
  EXPECT_EQ(v.notice, CallStoryNotice::Slow) << "measured against the first good sample";
}

TEST(CallConnectStoryTest, AnUnsettledCallShowsItsBarAfterTwoSecondsWithoutCallingItSlower) {
  CallConnectStory story;
  CallStoryInput in = At(0, Status::Connected, true, "direct", CallPathQuality::NoAudio, 0);
  in.group = true;
  story.Step(in);
  in.now_ms = 2'000;
  CallStoryView v = story.Step(in);
  EXPECT_TRUE(v.signal_shown);
  EXPECT_EQ(v.signal_tone, CallSignalTone::Poor);
  in = At(3'000, Status::Connected, true, "direct", CallPathQuality::Fair, 2);
  in.group = true;
  v = story.Step(in);
  EXPECT_EQ(v.notice, CallStoryNotice::None);
}

TEST(CallConnectStoryTest, NoAudioIsNotASlowerLine) {
  CallConnectStory story;
  story.Step(At(0, Status::Connected, true, "direct", CallPathQuality::Good, 4));
  const CallStoryView v = story.Step(At(1'000, Status::Connected, true, "direct", CallPathQuality::NoAudio, 0));
  EXPECT_EQ(v.signal_tone, CallSignalTone::Poor);
  EXPECT_EQ(v.notice, CallStoryNotice::None) << "no audio has its own hint";
}

TEST(CallConnectStoryTest, TheTimelineKeepsOnlyTheLatestMoveCloser) {
  CallConnectStory story;
  story.Step(At(0, Status::Connected, true, "circuit"));
  story.Step(At(3'000, Status::Connected, true, "direct"));
  story.Step(At(10'000, Status::Connected, true, "circuit"));
  const CallStoryView v = story.Step(At(20'000, Status::Connected, true, "punched"));
  int closer = 0;
  for (const CallStoryEvent& e : v.events) {
    closer += e.kind == CallStoryEvent::Kind::Closer ? 1 : 0;
  }
  EXPECT_EQ(closer, 1);
  EXPECT_EQ(v.events.back().call_ms, 20'000);
}

// A call found already live (the app came back to it) still says how it goes, without made-up timings.
TEST(CallConnectStoryTest, ACallFoundLiveListsHowItConnectedWithoutResultLine) {
  CallConnectStory story;
  const CallStoryView v = story.Step(At(50'000, Status::Connected, true, "circuit"));
  EXPECT_FALSE(v.result_shown);
  ASSERT_EQ(v.events.size(), 3u);
  EXPECT_EQ(v.events[1].kind, CallStoryEvent::Kind::Connected);
  EXPECT_EQ(v.events[1].route, CallRoute::Relayed);
  EXPECT_EQ(v.events[1].wall_ms, 50'000);
}

} // namespace
} // namespace pbr
