#include "feature/presenters/CallConnectStory.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

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
  EXPECT_EQ(v.stage, CallConnectStory::kStageCalling);
  EXPECT_FALSE(v.signal_shown);

  v = story.Step(At(4'000, Status::Connecting, false, ""));
  EXPECT_TRUE(v.connecting);
  EXPECT_EQ(v.stage, CallConnectStory::kStageAnswered) << "leaving Calling means the other side answered";
  v = story.Step(At(4'500, Status::Connecting, false, ""));
  EXPECT_EQ(v.stage, CallConnectStory::kStageMatching) << "the answer to live media is the line being matched";

  // Connected 1.8 s after the answer, around the network limits.
  v = story.Step(At(5'800, Status::Connected, true, "punched"));
  EXPECT_TRUE(v.connecting) << "the list still plays out";
  EXPECT_EQ(v.stage, CallConnectStory::kStageSecuring);
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

// #172: every step stays on screen at least 450 ms, however fast the real stages go; only the words wait.
TEST(CallConnectStoryTest, EachStepStaysAtLeastFourHundredFiftyMillisecondsInOrder) {
  CallConnectStory story;
  story.Step(At(1'000, Status::Calling));
  CallStoryView v = story.Step(At(1'100, Status::Connecting, false, ""));
  EXPECT_EQ(v.stage, CallConnectStory::kStageCalling) << "answered after 100 ms: calling stays";
  ASSERT_EQ(v.events.size(), 2u) << "the history has the answer at once";
  EXPECT_EQ(v.events[1].kind, CallStoryEvent::Kind::PeerAnswered);
  EXPECT_EQ(story.Step(At(1'449, Status::Connecting, false, "")).stage, CallConnectStory::kStageCalling);
  EXPECT_EQ(story.Step(At(1'450, Status::Connecting, false, "")).stage, CallConnectStory::kStageAnswered);
  EXPECT_EQ(story.Step(At(1'899, Status::Connecting, false, "")).stage, CallConnectStory::kStageAnswered);
  EXPECT_EQ(story.Step(At(1'900, Status::Connecting, false, "")).stage, CallConnectStory::kStageMatching);

  // Media is live at once: securing is shown its 450 ms, then the finished list.
  v = story.Step(At(1'950, Status::Connected, true, "direct"));
  EXPECT_EQ(v.stage, CallConnectStory::kStageMatching) << "matching has not had its 450 ms yet";
  EXPECT_EQ(story.Step(At(2'350, Status::Connected, true, "direct")).stage, CallConnectStory::kStageSecuring);
  EXPECT_EQ(story.Step(At(2'799, Status::Connected, true, "direct")).stage, CallConnectStory::kStageSecuring);
  v = story.Step(At(2'800, Status::Connected, true, "direct"));
  EXPECT_EQ(v.stage, CallConnectStory::kStageDone);
  EXPECT_TRUE(v.connecting);
}

// #172: after "connected" the whole list stays 1.5 s, then the call page takes over; the result line starts then.
TEST(CallConnectStoryTest, TheFinishedListHoldsOneAndAHalfSecondsBeforeTheCallPage) {
  CallConnectStory story;
  story.Step(At(0, Status::Calling));
  story.Step(At(500, Status::Connecting, false, ""));
  story.Step(At(1'000, Status::Connecting, false, ""));
  story.Step(At(1'500, Status::Connecting, false, ""));
  story.Step(At(2'000, Status::Connected, true, "direct"));
  CallStoryView v = story.Step(At(2'500, Status::Connected, true, "direct"));
  ASSERT_EQ(v.stage, CallConnectStory::kStageDone);
  EXPECT_TRUE(v.connecting);
  EXPECT_TRUE(v.result_shown);
  EXPECT_EQ(story.Step(At(3'999, Status::Connected, true, "direct")).connecting, true);
  v = story.Step(At(4'000, Status::Connected, true, "direct"));
  EXPECT_FALSE(v.connecting);
  EXPECT_TRUE(v.result_shown);
  EXPECT_FALSE(v.result_fading);
}

TEST(CallConnectStoryTest, AFailureDuringTheHoldEndsTheListAtOnce) {
  CallConnectStory story;
  story.Step(At(0, Status::Calling));
  story.Step(At(100, Status::Connecting, false, ""));
  story.Step(At(200, Status::Connected, true, "direct"));
  EXPECT_FALSE(story.Step(At(300, Status::Disconnected)).connecting);
}

TEST(CallConnectStoryTest, TheResultLineFadesAfterFourSecondsThenTheSignalBarShows) {
  CallConnectStory story;
  story.Step(At(0, Status::Calling));
  story.Step(At(500, Status::Connecting, false, ""));
  story.Step(At(1'000, Status::Connecting, false, ""));
  story.Step(At(1'500, Status::Connecting, false, ""));
  story.Step(At(2'000, Status::Connected, true, "direct"));
  story.Step(At(2'500, Status::Connected, true, "direct")); // securing -> done: the list holds 1.5 s
  const int64_t gone = 2'500 + CallConnectStory::kListHoldMs;
  CallStoryView v = story.Step(At(gone + 3'999, Status::Connected, true, "direct"));
  EXPECT_TRUE(v.result_shown);
  EXPECT_FALSE(v.result_fading);
  v = story.Step(At(gone + 4'000, Status::Connected, true, "direct"));
  EXPECT_TRUE(v.result_shown);
  EXPECT_TRUE(v.result_fading);
  EXPECT_FALSE(v.signal_shown);
  v = story.Step(At(gone + 4'400, Status::Connected, true, "direct"));
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
  for (const char* key : {"call.reconnect.restored",
                          "call.reconnect.offline",
                          "call.status.reconnecting",
                          "call.quality.hint.connecting_voice",
                          "call.quality.hint.connecting_video",
                          "call.quality.hint.retrying_voice",
                          "call.quality.hint.retrying_video",
                          "call.story.result",
                          "call.ring.awaiting.name",
                          "call.ring.step.received",
                          "call.ring.step.received_voice",
                          "call.ring.step.received_video",
                          "call.ring.step.joining",
                          "call.ring.step.joined",
                          "call.ring.step.verifying",
                          "call.story.step.calling",
                          "call.story.step.peer_answered",
                          "call.story.step.you_answered",
                          "call.story.step.matching",
                          "call.story.step.matched",
                          "call.story.step.matched_direct",
                          "call.story.step.matched_relayed",
                          "call.story.step.securing",
                          "call.story.step.connected",
                          "call.story.step.group_joined",
                          "call.story.step.group_calling",
                          "call.story.step.group_line",
                          "call.story.step.group_heard",
                          "call.story.sheet.title",
                          "call.story.sheet.lead",
                          "call.story.sheet.now",
                          "call.story.sheet.how",
                          "call.story.sheet.encryption",
                          "call.story.sheet.e2e",
                          "call.story.event.placed",
                          "call.story.event.answered",
                          "call.story.event.joined",
                          "call.story.event.peer_answered",
                          "call.story.event.encrypted",
                          "call.story.event.group_encrypted",
                          "call.story.event.group_connected",
                          "call.story.event.closer",
                          "call.story.seconds"}) {
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

// The phone page's steps (#172): an outgoing 1:1 call lists five, a call this device answered four (no "Calling"),
// a group three; the step on screen follows the story's stage, and "matched" names how the line went once known.
TEST(CallConnectStoryTest, AnOutgoingCallListsTheFiveStepsAndShowsItsStage) {
  CallStoryView v;
  v.outgoing = true;
  v.stage = CallConnectStory::kStageMatching;
  CallStorySteps steps = CallStoryLines(v, false);
  ASSERT_EQ(steps.keys.size(), 5u);
  EXPECT_EQ(steps.keys[0], "call.story.step.calling");
  EXPECT_EQ(steps.keys[1], "call.story.step.peer_answered");
  EXPECT_EQ(steps.keys[2], "call.story.step.matching") << "still matching: no route word yet";
  EXPECT_EQ(steps.keys[3], "call.story.step.securing");
  EXPECT_EQ(steps.keys[4], "call.story.step.connected");
  EXPECT_EQ(steps.shown, 2);

  v.stage = CallConnectStory::kStageSecuring;
  v.route = CallRoute::Relayed;
  steps = CallStoryLines(v, false);
  EXPECT_EQ(steps.keys[2], "call.story.step.matched_relayed");
  EXPECT_EQ(steps.shown, 3);
  v.route = CallRoute::AroundLimits;
  EXPECT_EQ(CallStoryLines(v, false).keys[2], "call.story.step.matched_direct");
  v.route = CallRoute::Unknown;
  EXPECT_EQ(CallStoryLines(v, false).keys[2], "call.story.step.matched");

  v.stage = CallConnectStory::kStageDone;
  EXPECT_EQ(CallStoryLines(v, false).shown, 5) << "every step done";
}

TEST(CallConnectStoryTest, ACallThisDeviceAnsweredListsFourStepsFromAnswered) {
  CallStoryView v;
  v.outgoing = false;
  v.stage = CallConnectStory::kStageAnswered;
  CallStorySteps steps = CallStoryLines(v, false);
  ASSERT_EQ(steps.keys.size(), 4u);
  EXPECT_EQ(steps.keys[0], "call.story.step.you_answered");
  EXPECT_EQ(steps.shown, 0) << "the answer is the first line on screen";
  v.stage = CallConnectStory::kStageSecuring;
  EXPECT_EQ(CallStoryLines(v, false).shown, 2);
  v.stage = CallConnectStory::kStageDone;
  EXPECT_EQ(CallStoryLines(v, false).shown, 4);
}

TEST(CallConnectStoryTest, AGroupCallListsThreeStepsCallingThenJoined) {
  CallStoryView v;
  v.stage = 0;
  CallStorySteps steps = CallStoryLines(v, true);
  ASSERT_EQ(steps.keys.size(), 3u);
  EXPECT_EQ(steps.keys[0], "call.story.step.group_calling");
  EXPECT_EQ(steps.keys[1], "call.story.step.group_line");
  EXPECT_EQ(steps.keys[2], "call.story.step.group_heard");
  EXPECT_EQ(steps.shown, 0);
  v.stage = 1;
  steps = CallStoryLines(v, true);
  EXPECT_EQ(steps.keys[0], "call.story.step.group_joined");
  EXPECT_EQ(steps.shown, 1);
}

TEST(CallConnectStoryTest, EveryStepHasWordsInEnglishAndChinese) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  CallStoryView v;
  std::vector<std::string> keys;
  for (const bool outgoing : {true, false}) {
    for (const CallRoute route : {CallRoute::Unknown, CallRoute::Direct, CallRoute::Relayed}) {
      for (const int stage : {0, 1, CallConnectStory::kStageSecuring}) {
        v.outgoing = outgoing;
        v.route = route;
        v.stage = stage;
        for (const bool group : {false, true}) {
          const CallStorySteps steps = CallStoryLines(v, group);
          keys.insert(keys.end(), steps.keys.begin(), steps.keys.end());
        }
      }
    }
  }
  for (const std::string& key : keys) {
    LocalizationService::Instance().SetPreferredLanguage("en");
    const std::string en = Tr(key);
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    EXPECT_NE(en, key);
    EXPECT_NE(Tr(key), en) << key;
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
}

// One reading of the path kind: the story's route and the desktop's path word agree on every kind.
TEST(CallConnectStoryTest, ThePathKindMapsToOneRouteAndOneWord) {
  EXPECT_EQ(CallRouteFromPathKind("direct"), CallRoute::Direct);
  EXPECT_EQ(CallRouteFromPathKind("punched"), CallRoute::AroundLimits);
  EXPECT_EQ(CallRouteFromPathKind("circuit"), CallRoute::Relayed);
  EXPECT_EQ(CallRouteFromPathKind("media_relay"), CallRoute::Relayed);
  EXPECT_EQ(CallRouteFromPathKind("relay"), CallRoute::Relayed);
  EXPECT_EQ(CallRouteFromPathKind(""), CallRoute::Unknown);
  EXPECT_STREQ(CallPathWordKey("direct"), "call.details.path.direct");
  EXPECT_STREQ(CallPathWordKey("punched"), "call.details.path.punched");
  EXPECT_STREQ(CallPathWordKey("circuit"), "call.details.path.circuit");
  EXPECT_STREQ(CallPathWordKey("media_relay"), "call.details.path.media_relay");
  EXPECT_STREQ(CallPathWordKey("relay"), "call.details.path.media_relay");
  EXPECT_STREQ(CallPathWordKey("sfu?"), "") << "an unknown kind shows no word";
}

} // namespace
} // namespace pbr
