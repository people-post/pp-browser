#include "lib/media/AudioResetHandoff.h"
#include "lib/media/SpeakerRouteRules.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace pbr {
namespace {

using namespace std::chrono_literals;

// The rules of handing an audio rebuild to SDL's main thread (SdlAudioReset_Android.cpp), here with a plain thread.

AudioResetResult Rebuilt(std::atomic<int>& rebuilds) {
  rebuilds.fetch_add(1);
  return AudioResetResult::Ok;
}

// The main thread runs it in time: its result.
TEST(AudioResetHandoffTest, ARebuildRunInTimeGivesItsResult) {
  AudioResetHandoff handoff;
  std::atomic<int> rebuilds{0};
  std::thread main([&] { handoff.Run([] { return true; }, [&](std::string*) { return Rebuilt(rebuilds); }); });
  std::string error;
  EXPECT_EQ(handoff.Wait(1s, &error), AudioResetResult::Ok);
  main.join();
  EXPECT_EQ(rebuilds.load(), 1);
}

// An audio stream opened before the rebuild starts: refused on the main thread, nothing is torn down.
TEST(AudioResetHandoffTest, AStreamOpenRightBeforeTheRebuildRefusesIt) {
  AudioResetHandoff handoff;
  std::atomic<int> rebuilds{0};
  std::thread main([&] { handoff.Run([] { return false; }, [&](std::string*) { return Rebuilt(rebuilds); }); });
  std::string error;
  EXPECT_EQ(handoff.Wait(1s, &error), AudioResetResult::Busy);
  main.join();
  EXPECT_EQ(rebuilds.load(), 0);
  EXPECT_FALSE(error.empty());
}

// The main thread did not get to it in time (paused, busy): given up, and it never runs later.
TEST(AudioResetHandoffTest, ARebuildNotStartedInTimeNeverRuns) {
  AudioResetHandoff handoff;
  std::atomic<int> rebuilds{0};
  std::string error;
  EXPECT_EQ(handoff.Wait(50ms, &error), AudioResetResult::Timeout);
  handoff.Run([] { return true; }, [&](std::string*) { return Rebuilt(rebuilds); }); // the main thread, late
  EXPECT_EQ(rebuilds.load(), 0) << "rebuilt after the waiter moved on";
  EXPECT_EQ(handoff.WaitUntilFinished(&error), AudioResetResult::Timeout);
}

// Started but slow (an SDL quit that blocks): the waiter is told it is stuck at the budget — bounded — and can then
// wait until it really returns before anything touches the audio stack again.
TEST(AudioResetHandoffTest, ARebuildRunningPastTheBudgetIsStuckUntilItReturns) {
  AudioResetHandoff handoff;
  std::atomic<bool> release{false};
  std::thread main([&] {
    handoff.Run([] { return true; },
                [&](std::string*) {
                  while (!release.load()) {
                    std::this_thread::sleep_for(2ms);
                  }
                  return AudioResetResult::Ok;
                });
  });
  std::string error;
  const auto from = std::chrono::steady_clock::now();
  EXPECT_EQ(handoff.Wait(100ms, &error), AudioResetResult::Stuck);
  EXPECT_LT(std::chrono::steady_clock::now() - from, 1s) << "the waiter was not bounded";
  release.store(true);
  EXPECT_EQ(handoff.WaitUntilFinished(&error), AudioResetResult::Ok);
  main.join();
}

// The call's session routes the speaker choice on its first activation; later only when the route is not the choice
// (the call before forgot its choice without routing), never re-pushing an unchanged route.
TEST(SpeakerRouteRulesTest, AnActivationRoutesOnlyWhenNeeded) {
  EXPECT_TRUE(PushSpeakerRouteOnActivate(/*was_active=*/false, RoutedSpeaker::Off, /*wanted=*/false));
  EXPECT_TRUE(PushSpeakerRouteOnActivate(false, RoutedSpeaker::On, true));
  EXPECT_FALSE(PushSpeakerRouteOnActivate(true, RoutedSpeaker::On, true)) << "a reopen re-pushed the same route";
  EXPECT_FALSE(PushSpeakerRouteOnActivate(true, RoutedSpeaker::Off, false));
  EXPECT_TRUE(PushSpeakerRouteOnActivate(true, RoutedSpeaker::On, false))
      << "hung up on speaker, next call stayed on it";
  EXPECT_TRUE(PushSpeakerRouteOnActivate(true, RoutedSpeaker::Unknown, false));
}

// A call ended: back to the earpiece at once only when the session is still up and no mic is open (a ringback put on
// the speaker, the call gone before media); with a mic open it waits for Deactivate after that mic closed.
TEST(SpeakerRouteRulesTest, AForgottenChoiceRoutesBackOnlyWithNoMicOpen) {
  EXPECT_TRUE(RouteBackWhenChoiceForgotten(/*session_active=*/true, /*mic_open=*/false));
  EXPECT_FALSE(RouteBackWhenChoiceForgotten(true, true)) << "routed under an open recording stream";
  EXPECT_FALSE(RouteBackWhenChoiceForgotten(false, false)) << "Deactivate already routed back";
  EXPECT_FALSE(RouteBackWhenChoiceForgotten(false, true));
}

} // namespace
} // namespace pbr
