#pragma once

#include <functional>

#include <ui/base/Input.h>
#include <ui/paint/RenderInterface.h>
#include <ui/base/SystemInterface.h>
#include <ui/base/Types.h>

#ifndef UI_SDL_VERSION_MAJOR
#define UI_SDL_VERSION_MAJOR 2
#endif

#if UI_SDL_VERSION_MAJOR >= 3
#include <SDL3/SDL.h>
#endif

using KeyDownCallback = bool (*)(ui::Context* context, ui::Input::KeyIdentifier key, int key_modifier,
                                 float native_dp_ratio, bool priority);
#if UI_SDL_VERSION_MAJOR >= 3
using PreProcessEventCallback = bool (*)(ui::Context* context, SDL_Event& event, bool& propagate_event);
// Called from SDL_AddEventWatch while Poll/WaitEvent is blocked in a modal resize/drag.
// Must SyncContext, Update layout, and Present so the OS does not stretch the last frame.
using LiveResizeRedrawCallback = std::function<void(ui::Context* context)>;
#endif

namespace Backend {

bool Initialize(const char* window_name, int width, int height, bool allow_resize, bool borderless = false);
void Shutdown();

ui::SystemInterface* GetSystemInterface();
ui::RenderInterface* GetRenderInterface();

void SyncContext(ui::Context* context);

// True when the window has a current GL context and a positive pixel size.
bool CanRender();

#if UI_SDL_VERSION_MAJOR >= 3
void SetPreProcessEventHandler(PreProcessEventCallback callback);
// Register context + redraw for live window resize (see SDL wiki AppFreezeDuringDrag).
void SetLiveResizeHandler(ui::Context* context, LiveResizeRedrawCallback callback);
SDL_Window* GetWindow();
// Rebuild GL resources and invalidate RmlUi GPU caches after SDL_EVENT_RENDER_DEVICE_RESET.
void RecoverAfterDeviceReset(ui::Context* context);
#endif

bool ProcessEvents(ui::Context* context, KeyDownCallback key_down_callback = nullptr, bool power_save = false);
void RequestExit();
/** Hide the product window immediately (close feel); SDL destroy still happens in Shutdown. */
void HideWindow();

// Thread-safe: end the idle wait (always pushes a wake event; do not coalesce-drop). Also sets the wake flag:
// the wait ignores a wake event whose flag is not set (EventLoopWake.h).
void WakeEventLoop();
// Thread-safe UI delivery: skip the next power-save idle wait + WakeEventLoop.
// AppRuntime::PostUI should call this (via SetUIWakeCallback), not WakeEventLoop alone.
void RequestForceFrame();
// perf.threads: RequestForceFrame calls (from any thread) since the last take; the take starts again from zero.
long long TakeForceFrameRequests();
// perf.threads: why ProcessEvents returned, since the last take (the take starts again from zero).
struct ProcessEventsCounts {
  long long flag = 0;           // pe_flag: the wake flag was set on entry
  long long event_first = 0;    // pe_event_first: the first poll already had an event
  long long event_waited = 0;   // pe_event_waited: an event arrived after waiting
  long long timeout = 0;        // pe_timeout: the idle wait ran to its deadline
  long long flag_waited = 0;    // pe_flag_waited: the wake flag was set while waiting
  long long wake_swallowed = 0; // pe_wake_swallowed: our own wake events dropped
};
ProcessEventsCounts TakeProcessEventsCounts();

void BeginFrame();
void PresentFrame();

} // namespace Backend
