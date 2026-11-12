#include "foundation/platform/ui/host/EventLoopWake.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

long long MsSince(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

struct TurnResult {
  long long ms = 0;
  std::vector<Uint32> events; // every event except our wake events, in the order handled
  EventLoopWake::Counts c;    // why it returned (the counts this turn added)
};

// What Backend::ProcessEvents does around the wake: take the first event (maybe waiting), then drain the queue,
// skipping our wake events.
TurnResult Turn(int timeout_ms) {
  TurnResult r;
  const auto started = Clock::now();
  SDL_Event ev;
  bool has_event = EventLoopWake::TakeFirstEvent(&ev, /*power_save=*/true, timeout_ms);
  while (has_event) {
    if (!EventLoopWake::SwallowIfWake(ev)) {
      r.events.push_back(ev.type);
    }
    has_event = SDL_PollEvent(&ev);
  }
  r.ms = MsSince(started);
  r.c = EventLoopWake::TakeCounts();
  return r;
}

// Judged by why the turn returned, not by milliseconds (a loaded CI machine schedules late): a turn meant to be woken
// gets a deadline far beyond any delay, and "woken" means it did not run to that deadline.
constexpr int kLong = 5000;
constexpr int kShort = 30;
bool Woken(const TurnResult& r) {
  return r.c.timeout == 0;
}
bool WaitedOut(const TurnResult& r) {
  return r.c.timeout == 1 && r.events.empty();
}

bool AnyUserEventQueued() {
  SDL_PumpEvents();
  return SDL_HasEvents(SDL_EVENT_USER, SDL_EVENT_LAST);
}

class EventLoopWakeTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS)) << SDL_GetError();
    EventLoopWake::Start();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    (void)EventLoopWake::TakeCounts();
  }
  void TearDown() override {
    EventLoopWake::SetBeforeFinalDropForTest(nullptr);
    EventLoopWake::Stop();
    SDL_Quit();
  }
};

TEST_F(EventLoopWakeTest, ARequestBeforeTheTurnEndsOneWaitOnly) {
  EventLoopWake::Request();
  const TurnResult first = Turn(kLong);
  EXPECT_EQ(first.c.flag, 1);
  EXPECT_TRUE(Woken(first));
  EXPECT_TRUE(WaitedOut(Turn(kShort))) << "the wake was used up: this turn waits out its deadline";
  EXPECT_FALSE(AnyUserEventQueued());
}

TEST_F(EventLoopWakeTest, ARequestDuringTheWaitEndsOneWaitOnly) {
  std::thread waker([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EventLoopWake::Request();
  });
  const TurnResult first = Turn(kLong);
  waker.join();
  EXPECT_EQ(first.c.flag, 0) << "it was waiting when the wake came";
  EXPECT_EQ(first.c.flag_waited + first.c.event_waited, 1);
  EXPECT_TRUE(Woken(first));
  // Before: the flag or the wake event was left for this turn, which then returned at once (two frames a wake).
  const TurnResult second = Turn(kShort);
  EXPECT_TRUE(WaitedOut(second));
  EXPECT_FALSE(AnyUserEventQueued());
  EXPECT_EQ(first.c.wake_swallowed + second.c.wake_swallowed, 1);
}

TEST_F(EventLoopWakeTest, TwoRequestsWithATurnBetweenEndOneTurnEach) {
  EventLoopWake::Request();
  const TurnResult a = Turn(kLong);
  EventLoopWake::Request();
  const TurnResult b = Turn(kLong);
  const TurnResult c = Turn(kShort);
  EXPECT_EQ(a.c.flag + b.c.flag, 2);
  EXPECT_TRUE(Woken(a));
  EXPECT_TRUE(Woken(b));
  EXPECT_TRUE(WaitedOut(c));
  EXPECT_EQ(a.c.wake_swallowed + b.c.wake_swallowed + c.c.wake_swallowed, 2);
}

TEST_F(EventLoopWakeTest, RealEventsAreKeptInOrder) {
  const Uint32 other_type = SDL_RegisterEvents(1); // someone else's user event: never dropped
  ASSERT_NE(other_type, 0u);
  auto push = [](Uint32 type) {
    SDL_Event ev{};
    ev.type = type;
    if (type == SDL_EVENT_KEY_DOWN) {
      ev.key.key = SDLK_A;
      ev.key.down = true;
    }
    ASSERT_TRUE(SDL_PushEvent(&ev)) << SDL_GetError();
  };
  push(other_type);
  EventLoopWake::Request();
  push(SDL_EVENT_KEY_DOWN);
  EventLoopWake::Request();
  push(other_type);
  const TurnResult r = Turn(kLong);
  EXPECT_TRUE(Woken(r));
  EXPECT_EQ(r.events, (std::vector<Uint32>{other_type, SDL_EVENT_KEY_DOWN, other_type}));
  EXPECT_FALSE(AnyUserEventQueued());
}

TEST_F(EventLoopWakeTest, ARealEventDuringTheWaitIsHandledWithItsWake) {
  std::thread poster([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    SDL_Event ev{};
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = SDLK_B;
    ev.key.down = true;
    SDL_PushEvent(&ev);
    EventLoopWake::Request();
  });
  const TurnResult first = Turn(kLong);
  poster.join();
  // The key may arrive in this turn on its own (the wake follows in the next one); never lost, never twice.
  TurnResult second = Turn(kShort);
  std::vector<Uint32> all = first.events;
  all.insert(all.end(), second.events.begin(), second.events.end());
  EXPECT_EQ(all, (std::vector<Uint32>{SDL_EVENT_KEY_DOWN}));
  EXPECT_TRUE(Woken(first));
  EXPECT_FALSE(AnyUserEventQueued());
}

TEST_F(EventLoopWakeTest, AFlagOnlyMarkEndsOneWait) {
  EventLoopWake::MarkFrame(); // SyncContext: no event pushed
  EXPECT_TRUE(Woken(Turn(kLong)));
  EXPECT_TRUE(WaitedOut(Turn(kShort)));
}

TEST_F(EventLoopWakeTest, AStaleWakeEventDoesNotEndTheNextWait) {
  // The race a device hits on most video wakes: the waiter sees the flag (notified first), drops the queued wake
  // events and returns — then the requester's push lands. That late event must not end the next wait.
  EventLoopWake::Request();
  SDL_Event queued{};
  ASSERT_EQ(SDL_PeepEvents(&queued, 1, SDL_PEEKEVENT, SDL_EVENT_USER, SDL_EVENT_LAST), 1);
  const Uint32 wake_type = queued.type;
  const TurnResult first = Turn(kLong);
  EXPECT_TRUE(Woken(first));
  SDL_Event late{};
  late.type = wake_type;
  ASSERT_TRUE(SDL_PushEvent(&late));
  const TurnResult second = Turn(kShort);
  EXPECT_TRUE(WaitedOut(second));
  EXPECT_FALSE(AnyUserEventQueued());
  EXPECT_EQ(first.c.wake_swallowed + second.c.wake_swallowed, 2);
}

TEST_F(EventLoopWakeTest, ARequestWithoutAWakeTypeStillEndsTheNextWait) {
  // SDL_RegisterEvents failed, or after Stop: no wake event can be pushed, but the flag still forces the next turn.
  EventLoopWake::Stop();
  EventLoopWake::Request();
  const TurnResult r = Turn(kLong);
  EXPECT_EQ(r.c.flag, 1);
  EXPECT_TRUE(Woken(r));
  EXPECT_TRUE(WaitedOut(Turn(kShort)));
}

std::atomic<int> g_hook_requests{0};

TEST_F(EventLoopWakeTest, AWakeBetweenTheFinalClearAndTheDropIsNotLost) {
  // The wait ran out; its flag is cleared; a wake lands (flag set, event pushed) just before the queued wake events
  // are dropped — so its event goes. The flag it left must still end the next wait.
  g_hook_requests = 0;
  EventLoopWake::SetBeforeFinalDropForTest([] {
    if (g_hook_requests.fetch_add(1) == 0) {
      EventLoopWake::Request();
    }
  });
  const TurnResult first = Turn(kShort);
  EventLoopWake::SetBeforeFinalDropForTest(nullptr);
  ASSERT_EQ(g_hook_requests.load(), 1);
  EXPECT_EQ(first.c.timeout, 1);
  EXPECT_EQ(first.c.wake_swallowed, 1) << "its wake event was dropped";
  const TurnResult second = Turn(kLong);
  EXPECT_EQ(second.c.flag, 1) << "the flag carried the wake";
  EXPECT_TRUE(Woken(second));
  EXPECT_TRUE(WaitedOut(Turn(kShort))) << "one extra turn at most";
}

bool SDLCALL RequestFromWatch(void* userdata, SDL_Event* event) {
  if (event->type == *static_cast<Uint32*>(userdata)) {
    EventLoopWake::Request(); // pushes our wake event from inside SDL's own push of this one
  }
  return true;
}

TEST_F(EventLoopWakeTest, ARequestFromAnEventWatchDoesNotDeadlock) {
  // BrowserHost's live-resize watch calls RequestForceFrame from inside SDL's event dispatch.
  Uint32 watched = SDL_RegisterEvents(1);
  ASSERT_NE(watched, 0u);
  ASSERT_TRUE(SDL_AddEventWatch(RequestFromWatch, &watched));
  SDL_Event ev{};
  ev.type = watched;
  ASSERT_TRUE(SDL_PushEvent(&ev)); // returns: no deadlock
  const TurnResult r = Turn(kLong);
  SDL_RemoveEventWatch(RequestFromWatch, &watched);
  EXPECT_EQ(r.c.flag, 1);
  EXPECT_EQ(r.events, (std::vector<Uint32>{watched}));
  EXPECT_FALSE(AnyUserEventQueued());
}

TEST_F(EventLoopWakeTest, NoWakeIsLostUnderConcurrentRequests) {
  // A producer posts "work" then requests a frame, like AppRuntime::PostUI. The loop serves whatever was posted
  // when its turn returns. A lost wake shows as a turn that started with work pending and still ran to its deadline.
  constexpr int kRequests = 1000;
  std::atomic<int> posted{0};
  std::atomic<bool> done{false};
  std::thread producer([&] {
    std::mt19937 rng(7);
    for (int i = 0; i < kRequests; ++i) {
      posted.fetch_add(1, std::memory_order_relaxed);
      EventLoopWake::Request();
      if (rng() % 4 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500));
      }
    }
    done.store(true);
  });
  int served = 0;
  int pending_turns_run_out = 0;
  const auto started = Clock::now();
  while (served < kRequests && MsSince(started) < 60000) {
    const bool pending_at_start = posted.load() > served;
    const TurnResult r = Turn(kLong);
    if (pending_at_start && !Woken(r)) {
      ++pending_turns_run_out;
    }
    served = posted.load(); // the turn's work: everything posted before it returned
  }
  producer.join();
  EXPECT_TRUE(done.load());
  EXPECT_EQ(served, kRequests);
  EXPECT_EQ(pending_turns_run_out, 0);
  // Settle: at most one extra turn is left over, then the loop waits again and nothing of ours is queued.
  int quick_turns = 0;
  while (Woken(Turn(kShort)) && quick_turns < 5) {
    ++quick_turns;
  }
  EXPECT_LE(quick_turns, 1);
  EXPECT_FALSE(AnyUserEventQueued());
}

} // namespace
