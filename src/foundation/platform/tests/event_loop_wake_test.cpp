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
  return r;
}

bool AnyUserEventQueued() {
  SDL_PumpEvents();
  return SDL_HasEvents(SDL_EVENT_USER, SDL_EVENT_LAST);
}

// Generous bounds: "returned at once" < 40 ms, "waited" >= timeout - 10 ms.
constexpr long long kAtOnceMs = 40;

class EventLoopWakeTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS)) << SDL_GetError();
    EventLoopWake::Start();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    (void)EventLoopWake::TakeCounts();
  }
  void TearDown() override {
    EventLoopWake::Stop();
    SDL_Quit();
  }
};

TEST_F(EventLoopWakeTest, ARequestBeforeTheTurnEndsOneWaitOnly) {
  EventLoopWake::Request();
  EXPECT_LT(Turn(1000).ms, kAtOnceMs);
  EXPECT_GE(Turn(150).ms, 140); // the wake was used up: this turn waits out its deadline
  EXPECT_FALSE(AnyUserEventQueued());
}

TEST_F(EventLoopWakeTest, ARequestDuringTheWaitEndsOneWaitOnly) {
  std::thread waker([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EventLoopWake::Request();
  });
  const TurnResult first = Turn(1500);
  waker.join();
  EXPECT_LT(first.ms, 500);
  EXPECT_GE(first.ms, 20);
  // Before: the flag or the wake event was left for this turn, which then returned at once (two frames a wake).
  EXPECT_GE(Turn(150).ms, 140);
  EXPECT_FALSE(AnyUserEventQueued());
  const EventLoopWake::Counts c = EventLoopWake::TakeCounts();
  EXPECT_EQ(c.flag, 0);
  EXPECT_EQ(c.flag_waited + c.event_waited, 1);
  EXPECT_EQ(c.timeout, 1);
  EXPECT_EQ(c.wake_swallowed, 1);
}

TEST_F(EventLoopWakeTest, TwoRequestsWithATurnBetweenEndOneTurnEach) {
  EventLoopWake::Request();
  EXPECT_LT(Turn(1000).ms, kAtOnceMs);
  EventLoopWake::Request();
  EXPECT_LT(Turn(1000).ms, kAtOnceMs);
  EXPECT_GE(Turn(150).ms, 140);
  const EventLoopWake::Counts c = EventLoopWake::TakeCounts();
  EXPECT_EQ(c.flag, 2);
  EXPECT_EQ(c.wake_swallowed, 2);
  EXPECT_EQ(c.timeout, 1);
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
  const TurnResult r = Turn(1000);
  EXPECT_LT(r.ms, kAtOnceMs);
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
  const TurnResult first = Turn(1500);
  poster.join();
  // The key may arrive in this turn on its own (the wake follows in the next one); never lost, never twice.
  TurnResult second = Turn(150);
  std::vector<Uint32> all = first.events;
  all.insert(all.end(), second.events.begin(), second.events.end());
  EXPECT_EQ(all, (std::vector<Uint32>{SDL_EVENT_KEY_DOWN}));
  EXPECT_LT(first.ms, 500);
  EXPECT_FALSE(AnyUserEventQueued());
}

TEST_F(EventLoopWakeTest, AFlagOnlyMarkEndsOneWait) {
  EventLoopWake::MarkFrame(); // SyncContext: no event pushed
  EXPECT_LT(Turn(1000).ms, kAtOnceMs);
  EXPECT_GE(Turn(150).ms, 140);
}

TEST_F(EventLoopWakeTest, AStaleWakeEventDoesNotEndTheNextWait) {
  // The race a device hits on most video wakes: the waiter sees the flag (notified first), drops the queued wake
  // events and returns — then the requester's push lands. That late event must not end the next wait.
  EventLoopWake::Request();
  SDL_Event queued{};
  ASSERT_EQ(SDL_PeepEvents(&queued, 1, SDL_PEEKEVENT, SDL_EVENT_USER, SDL_EVENT_LAST), 1);
  const Uint32 wake_type = queued.type;
  EXPECT_LT(Turn(1000).ms, kAtOnceMs);
  SDL_Event late{};
  late.type = wake_type;
  ASSERT_TRUE(SDL_PushEvent(&late));
  EXPECT_GE(Turn(150).ms, 140);
  EXPECT_FALSE(AnyUserEventQueued());
  EXPECT_EQ(EventLoopWake::TakeCounts().wake_swallowed, 2);
}

TEST_F(EventLoopWakeTest, NoWakeIsLostUnderConcurrentRequests) {
  // A producer posts "work" then requests a frame, like AppRuntime::PostUI. The loop serves whatever was posted
  // when its turn returns. A lost wake shows as a turn that started with work pending and still waited.
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
  long long worst_pending_wait_ms = 0;
  const auto started = Clock::now();
  while (served < kRequests && MsSince(started) < 20000) {
    const bool pending_at_start = posted.load() > served;
    const TurnResult r = Turn(500);
    if (pending_at_start && r.ms > worst_pending_wait_ms) {
      worst_pending_wait_ms = r.ms;
    }
    served = posted.load(); // the turn's work: everything posted before it returned
  }
  producer.join();
  EXPECT_TRUE(done.load());
  EXPECT_EQ(served, kRequests);
  EXPECT_LT(worst_pending_wait_ms, 200);
  // Settle: at most one extra turn is left over, then the loop waits again and nothing of ours is queued.
  int quick_turns = 0;
  while (Turn(100).ms < 90 && quick_turns < 5) {
    ++quick_turns;
  }
  EXPECT_LE(quick_turns, 1);
  EXPECT_FALSE(AnyUserEventQueued());
}

} // namespace
