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
#endif

namespace Backend {

bool Initialize(const char* window_name, int width, int height, bool allow_resize);
void Shutdown();

Rml::SystemInterface* GetSystemInterface();
Rml::RenderInterface* GetRenderInterface();

void SyncContext(Rml::Context* context);

// True when the window has a current GL context and a positive pixel size.
bool CanRender();

#if RMLUI_SDL_VERSION_MAJOR >= 3
void SetPreProcessEventHandler(PreProcessEventCallback callback);
SDL_Window* GetWindow();
// Rebuild GL resources and invalidate RmlUi GPU caches after SDL_EVENT_RENDER_DEVICE_RESET.
void RecoverAfterDeviceReset(Rml::Context* context);
#endif

bool ProcessEvents(Rml::Context* context, KeyDownCallback key_down_callback = nullptr, bool power_save = false);
void RequestExit();

// Thread-safe: breaks SDL_WaitEventTimeout so posted UI work can run without waiting for input.
void WakeEventLoop();

void BeginFrame();
void PresentFrame();

} // namespace Backend
