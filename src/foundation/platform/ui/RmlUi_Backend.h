#pragma once

#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

#ifndef RMLUI_SDL_VERSION_MAJOR
#define RMLUI_SDL_VERSION_MAJOR 2
#endif

#if RMLUI_SDL_VERSION_MAJOR >= 3
#include <SDL3/SDL.h>
#endif

using KeyDownCallback = bool (*)(Rml::Context* context, Rml::Input::KeyIdentifier key, int key_modifier, float native_dp_ratio, bool priority);
#if RMLUI_SDL_VERSION_MAJOR >= 3
using PreProcessEventCallback = bool (*)(Rml::Context* context, SDL_Event& event, bool& propagate_event);
// Called from SDL_AddEventWatch while Poll/WaitEvent is blocked in a modal resize/drag.
// Must SyncContext, Update layout, and Present so the OS does not stretch the last frame.
using LiveResizeRedrawCallback = void (*)(Rml::Context* context);
#endif

namespace Backend {

bool Initialize(const char* window_name, int width, int height, bool allow_resize,
                bool borderless = false);
void Shutdown();

Rml::SystemInterface* GetSystemInterface();
Rml::RenderInterface* GetRenderInterface();

void SyncContext(Rml::Context* context);

// True when the window has a current GL context and a positive pixel size.
bool CanRender();

#if RMLUI_SDL_VERSION_MAJOR >= 3
void SetPreProcessEventHandler(PreProcessEventCallback callback);
// Register context + redraw for live window resize (see SDL wiki AppFreezeDuringDrag).
void SetLiveResizeHandler(Rml::Context* context, LiveResizeRedrawCallback callback);
SDL_Window* GetWindow();
// Rebuild GL resources and invalidate RmlUi GPU caches after SDL_EVENT_RENDER_DEVICE_RESET.
void RecoverAfterDeviceReset(Rml::Context* context);
#endif

bool ProcessEvents(Rml::Context* context, KeyDownCallback key_down_callback = nullptr, bool power_save = false);
void RequestExit();

// Thread-safe: push an SDL user event (always push; do not coalesce-drop).
void WakeEventLoop();
// Thread-safe UI delivery: skip the next power-save idle wait + WakeEventLoop.
// AppRuntime::PostUI should call this (via SetUIWakeCallback), not WakeEventLoop alone.
void RequestForceFrame();

void BeginFrame();
void PresentFrame();

} // namespace Backend
