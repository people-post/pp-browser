#pragma once

#include <SDL3/SDL.h>

/**
 * The main loop's wake, without a window (so tests drive it with SDL_INIT_EVENTS only).
 *
 * A wake leaves two things: a flag (seen by the idle wait at once, through a condition variable) and an SDL
 * user event (so a wait blocked in SDL notices it too). Both are consumed together, so one wake makes one
 * ProcessEvents return, not two (DEV PLAN-call-frame-cost: 171 video wakes drew 364 frames per 10 s).
 *
 * Only our own wake event type is ever dropped; every other event stays queued in its order.
 * A wake is never lost: the flag is cleared only while ProcessEvents has not yet returned, so the turn that
 * follows (UI tasks, Update, draw) serves it; a wake arriving after the clear leaves the flag set and the
 * next ProcessEvents does not wait.
 */
namespace EventLoopWake {

struct Counts {
  long long flag = 0;           // ProcessEvents found the flag set on entry
  long long event_first = 0;    // the first poll already had an event (power-save path)
  long long event_waited = 0;   // an event arrived after waiting
  long long timeout = 0;        // the wait ran to its deadline
  long long flag_waited = 0;    // the flag was set while waiting
  long long wake_swallowed = 0; // our wake events dropped (taken with the flag, or met among the events)
};

// After SDL_Init with the events subsystem: registers the wake event type. Stop() forgets it.
void Start();
void Stop();

// Thread-safe. Flag + end the idle wait + push a wake event (Backend::RequestForceFrame, Backend::WakeEventLoop).
// A wake event alone would not end a wait: the wait goes by the flag (see TakeFirstEvent).
void Request();
// Thread-safe. Flag only: the next ProcessEvents does not wait (Backend::SyncContext, device reset).
void MarkFrame();

// The start of a ProcessEvents turn: returns whether *ev holds the first event to handle. With power_save and
// no flag set, waits up to timeout_ms for a real event or the flag; our wake events met while waiting are
// dropped without ending the wait. Those still queued when the wait ends are dropped together with the flag.
bool TakeFirstEvent(SDL_Event* ev, bool power_save, int timeout_ms);
// True for our wake event (counted as swallowed): the caller skips it.
bool SwallowIfWake(const SDL_Event& ev);

// perf.threads: the counts since the last take; the take starts again from zero.
Counts TakeCounts();

// Tests only: called in TakeFirstEvent between the wait's final flag clear and the drop of the queued wake events.
void SetBeforeFinalDropForTest(void (*hook)());

} // namespace EventLoopWake
