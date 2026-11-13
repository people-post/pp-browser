#include "EventLoopWake.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace EventLoopWake {
namespace {

// Custom SDL user event that ends a wait blocked in SDL when UI work is posted.
std::atomic<Uint32> g_wake_event_type{0};
/** Cross-thread: Request / MarkFrame may set it from any thread; only TakeFirstEvent clears it. */
std::atomic<bool> g_flag{false};
/**
 * The idle wait sleeps on this, not SDL_Delay: Request from another thread (a posted UI task, a new
 * call video frame) ends the wait at once instead of at the end of a 50 ms slice — at video rate a slice-bound
 * wake drew at most ~20 fps, unevenly. SDL input is still polled every slice.
 */
std::mutex g_idle_wait_mu;
std::condition_variable g_idle_wait_cv;

std::atomic<void (*)()> g_before_final_drop{nullptr}; // tests only

std::atomic<long long> g_flag_count{0};
std::atomic<long long> g_event_first{0};
std::atomic<long long> g_event_waited{0};
std::atomic<long long> g_timeout{0};
std::atomic<long long> g_flag_waited{0};
std::atomic<long long> g_wake_swallowed{0};

void Count(std::atomic<long long>& c, long long n = 1) {
  c.fetch_add(n, std::memory_order_relaxed);
}

// Drops the wake events already queued — only our type, nothing else is taken out. Called right after the
// flag was cleared: a Request sets its flag before it pushes, so a wake event taken here belongs to a request
// whose flag this turn already took (served now) or that is still set (the next turn does not wait).
void DropQueuedWakeEvents() {
  const Uint32 type = g_wake_event_type.load(std::memory_order_relaxed);
  if (type == 0) {
    return;
  }
  SDL_Event batch[16];
  int got = 0;
  do {
    got = SDL_PeepEvents(batch, 16, SDL_GETEVENT, type, type);
    if (got > 0) {
      Count(g_wake_swallowed, got);
    }
  } while (got == 16);
}

// The wait's poll: our wake events are taken and dropped, the first other event is returned. A wake event never
// ends the wait by itself — its request set the flag before pushing it, so a new wake shows as the flag (checked
// next), and a wake event whose flag a turn already took (pushed just after that turn's drop) is stale: it used
// to make the following turn return at once, a second frame for one wake.
bool PollSkippingWakeEvents(SDL_Event* ev) {
  const Uint32 type = g_wake_event_type.load(std::memory_order_relaxed);
  while (SDL_PollEvent(ev)) {
    if (type == 0 || ev->type != type) {
      return true;
    }
    Count(g_wake_swallowed);
  }
  return false;
}

} // namespace

void Start() {
  g_wake_event_type.store(SDL_RegisterEvents(1), std::memory_order_relaxed);
  g_flag.store(false, std::memory_order_relaxed);
}

void Stop() {
  g_wake_event_type.store(0, std::memory_order_relaxed);
  g_flag.store(false, std::memory_order_relaxed);
}

void MarkFrame() {
  g_flag.store(true, std::memory_order_release);
}

void Request() {
  // The flag first, then the event: whoever takes the event finds the flag already set (or already taken). The flag
  // and the notify do not need the wake event: with no type (SDL_RegisterEvents failed, or after Stop — Start clears
  // the flag) the next wait still ends at once, as the force flag always did.
  g_flag.store(true, std::memory_order_release);
  {
    std::lock_guard lock(g_idle_wait_mu); // a waiter between its check and its sleep sees the flag
  }
  g_idle_wait_cv.notify_one();
  const Uint32 type = g_wake_event_type.load(std::memory_order_relaxed);
  if (type == 0) {
    return;
  }
  // Always push a wake event: coalescing on a "pending" flag once dropped wakes, leaving UI tasks
  // (SyncLayout, call ring) queued until the next real input.
  SDL_Event ev{};
  ev.type = type;
  SDL_PushEvent(&ev);
}

bool TakeFirstEvent(SDL_Event* ev, bool power_save, int timeout_ms) {
  // Never use SDL_WaitEventTimeout for idle — on some X11/SDL builds it ignored the timeout
  // until real input, so SyncLayout / call ring stayed invisible until Sessions was opened.
  if (g_flag.exchange(false, std::memory_order_acq_rel)) {
    Count(g_flag_count);
    DropQueuedWakeEvents();
    return SDL_PollEvent(ev);
  }
  if (!power_save) {
    return SDL_PollEvent(ev);
  }
  if (timeout_ms < 0) {
    timeout_ms = 0;
  }
  const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(timeout_ms);
  SDL_PumpEvents();
  bool has_event = PollSkippingWakeEvents(ev);
  if (has_event) {
    Count(g_event_first);
  }
  while (!has_event) {
    // Mid-idle Request from another thread — exit without waiting out the slice.
    if (g_flag.exchange(false, std::memory_order_acq_rel)) {
      Count(g_flag_waited);
      SDL_PumpEvents();
      DropQueuedWakeEvents();
      return SDL_PollEvent(ev);
    }
    const Uint64 now = SDL_GetTicks();
    if (now >= deadline) {
      Count(g_timeout);
      break;
    }
    Uint64 remaining = deadline - now;
    if (remaining > 50) {
      remaining = 50; // ≤50ms slices so PushEvent / Request are observed promptly
    }
    {
      std::unique_lock lock(g_idle_wait_mu);
      g_idle_wait_cv.wait_for(lock, std::chrono::milliseconds(remaining),
                              [] { return g_flag.load(std::memory_order_acquire); });
    }
    SDL_PumpEvents();
    has_event = PollSkippingWakeEvents(ev);
    if (has_event) {
      Count(g_event_waited);
    }
  }
  // The wait ended on a real event or at the deadline: this turn serves the flag and the wake events queued so
  // far. A wake after this line keeps its flag set.
  g_flag.exchange(false, std::memory_order_acq_rel);
  if (void (*hook)() = g_before_final_drop.load(std::memory_order_relaxed)) {
    hook();
  }
  DropQueuedWakeEvents();
  return has_event;
}

bool SwallowIfWake(const SDL_Event& ev) {
  const Uint32 type = g_wake_event_type.load(std::memory_order_relaxed);
  if (type == 0 || ev.type != type) {
    return false;
  }
  Count(g_wake_swallowed);
  return true;
}

void SetBeforeFinalDropForTest(void (*hook)()) {
  g_before_final_drop.store(hook, std::memory_order_relaxed);
}

Counts TakeCounts() {
  Counts c;
  c.flag = g_flag_count.exchange(0, std::memory_order_relaxed);
  c.event_first = g_event_first.exchange(0, std::memory_order_relaxed);
  c.event_waited = g_event_waited.exchange(0, std::memory_order_relaxed);
  c.timeout = g_timeout.exchange(0, std::memory_order_relaxed);
  c.flag_waited = g_flag_waited.exchange(0, std::memory_order_relaxed);
  c.wake_swallowed = g_wake_swallowed.exchange(0, std::memory_order_relaxed);
  return c;
}

} // namespace EventLoopWake
