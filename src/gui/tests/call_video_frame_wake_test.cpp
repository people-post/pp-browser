#include "gui/CallVideoFrameWake.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kT0 = 1'000'000;
constexpr int64_t kWindow = kCallVideoWakeMinIntervalMs;

// A fake clock and a fake coordinator: scheduled callbacks run only when the test fires them.
struct Harness {
  int64_t now = kT0;
  int wakes = 0;
  struct Timer {
    int64_t delay_ms;
    std::function<void()> fire;
  };
  std::vector<Timer> timers;
  std::shared_ptr<CallVideoFrameWake> gate = CallVideoFrameWake::Create(
      [this] { return now; }, [this](int64_t delay_ms, std::function<void()> fire) {
        timers.push_back({delay_ms, std::move(fire)});
      },
      [this] { ++wakes; });

  void FireTimers() {
    std::vector<Timer> due;
    due.swap(timers);
    for (Timer& t : due) {
      t.fire();
    }
  }
};

TEST(CallVideoFrameWakeTest, TheWindowIsOneFrameAtFifteenFps) {
  EXPECT_EQ(kWindow, 66);
}

TEST(CallVideoFrameWakeTest, ManyFramesInOneWindowWakeOnceAtOnce) {
  Harness h;
  h.gate->OnFrame();
  EXPECT_EQ(h.wakes, 1) << "the window's first frame wakes the loop at once";
  for (int64_t t : {5, 20, 40, 65}) {
    h.now = kT0 + t;
    h.gate->OnFrame();
  }
  EXPECT_EQ(h.wakes, 1) << "later frames in the same window do not wake at once";
  ASSERT_EQ(h.timers.size(), 1u) << "one catch-up timer, not one per frame";
  EXPECT_EQ(h.timers[0].delay_ms, kWindow - 5) << "armed for the rest of the window";
}

TEST(CallVideoFrameWakeTest, AFrameHeldBackInTheWindowIsWokenForWhenTheWindowEnds) {
  Harness h;
  h.gate->OnFrame();
  h.now = kT0 + 30;
  h.gate->OnFrame(); // the last frame before the peer stops sending: nothing else will come
  EXPECT_EQ(h.wakes, 1);
  h.now = kT0 + kWindow;
  h.FireTimers();
  EXPECT_EQ(h.wakes, 2) << "the held-back frame is drawn when the window ends";
  EXPECT_TRUE(h.timers.empty());
}

TEST(CallVideoFrameWakeTest, ALoneFrameInItsWindowAddsNoCatchUpWake) {
  Harness h;
  h.gate->OnFrame();
  EXPECT_TRUE(h.timers.empty());
  h.now = kT0 + kWindow;
  h.FireTimers();
  EXPECT_EQ(h.wakes, 1);
}

TEST(CallVideoFrameWakeTest, AFrameAfterTheWindowWakesAtOnceAndVoidsTheCatchUp) {
  Harness h;
  h.gate->OnFrame();
  h.now = kT0 + 10;
  h.gate->OnFrame(); // held back, catch-up armed
  h.now = kT0 + kWindow + 1;
  h.gate->OnFrame(); // the coordinator ran late: this frame wakes at once and draws the held one too
  EXPECT_EQ(h.wakes, 2);
  h.FireTimers();
  EXPECT_EQ(h.wakes, 2) << "the late catch-up finds nothing held back";
}

TEST(CallVideoFrameWakeTest, ACatchUpWakeStartsTheNextWindow) {
  Harness h;
  h.gate->OnFrame();
  h.now = kT0 + 30;
  h.gate->OnFrame();
  h.now = kT0 + kWindow;
  h.FireTimers();
  ASSERT_EQ(h.wakes, 2);
  h.now = kT0 + kWindow + 10; // 10 ms after the catch-up wake: held, a new catch-up armed
  h.gate->OnFrame();
  EXPECT_EQ(h.wakes, 2);
  ASSERT_EQ(h.timers.size(), 1u);
  EXPECT_EQ(h.timers[0].delay_ms, kWindow - 10);
}

TEST(CallVideoFrameWakeTest, SteadyFifteenFpsFromTwoStreamsWakesAboutFifteenTimesASecond) {
  Harness h;
  // Two peers at 15 fps, out of step (0 ms and 33 ms phase). Timers fire on time.
  std::vector<int64_t> frames;
  for (int64_t t = 0; t < 1000; t += 66) {
    frames.push_back(t);
    frames.push_back(t + 33);
  }
  int64_t next_timer_at = -1;
  for (int64_t t = 0; t <= 1100; ++t) {
    h.now = kT0 + t;
    if (next_timer_at == t) {
      next_timer_at = -1;
      h.FireTimers();
    }
    for (int64_t f : frames) {
      if (f == t) {
        h.gate->OnFrame();
      }
    }
    if (!h.timers.empty() && next_timer_at < 0) {
      next_timer_at = t + h.timers.back().delay_ms;
    }
  }
  EXPECT_LE(h.wakes, 1000 / kWindow + 2);
  EXPECT_GE(h.wakes, 1000 / kWindow - 1);
}

TEST(CallVideoFrameWakeTest, ACatchUpFiringAfterTheOwnerIsGoneIsHarmless) {
  int wakes = 0;
  std::vector<std::function<void()>> timers;
  int64_t now = kT0;
  {
    auto gate = CallVideoFrameWake::Create(
        [&now] { return now; },
        [&timers](int64_t, std::function<void()> fire) { timers.push_back(std::move(fire)); }, [&wakes] { ++wakes; });
    gate->OnFrame();
    now += 10;
    gate->OnFrame();
  } // the controller and its media callback are gone; the coordinator still holds the timer
  ASSERT_EQ(timers.size(), 1u);
  timers[0]();
  EXPECT_EQ(wakes, 2) << "the timer owns what it touches: one last wake, no dangling access";
}

TEST(CallVideoFrameWakeTest, TheAppClockIsTheMonotonicOne) {
  // The window is timed on the steady clock (the tests above inject their own): a wall-clock time sync cannot move it.
  const int64_t before = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  const int64_t now = CallVideoFrameWake::SteadyNowMs();
  const int64_t after = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  EXPECT_GE(now, before);
  EXPECT_LE(now, after);
}

TEST(CallVideoFrameWakeTest, AClockThatStepsBackDoesNotHoldFramesForever) {
  Harness h;
  h.gate->OnFrame();
  h.now = kT0 - 60'000; // an injected clock set back a minute
  h.gate->OnFrame();
  EXPECT_EQ(h.wakes, 2) << "a new window starts; frames are not held until the clock catches up";
  h.now = kT0 - 60'000 + 10;
  h.gate->OnFrame();
  EXPECT_EQ(h.wakes, 2) << "and is paced as usual from there";
  ASSERT_EQ(h.timers.size(), 1u);
  EXPECT_EQ(h.timers[0].delay_ms, kWindow - 10);
}

} // namespace
} // namespace pbr
