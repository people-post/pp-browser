#include "foundation/platform/ui/DesktopWindowChrome.h"

#include "foundation/platform/Platform.h"
#include "foundation/platform/desktop/LocalNotifierImpl.h"
#include "foundation/platform/desktop/WindowCorners.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

#include "foundation/platform/ui/RmlUi_Backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include "foundation/platform/desktop/WindowChrome_Darwin.h"
#endif
#endif

#if UI_SDL_VERSION_MAJOR >= 3
#include <SDL3/SDL.h>
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif
#endif

namespace pbr {
namespace {

#if UI_SDL_VERSION_MAJOR >= 3
struct HitTestLayout {
  int titlebar_height_win = 36;
  int controls_width_win = 120;
  int edge_margin_win = 5;
  bool controls_leading = false;
};

HitTestLayout g_layout{};
bool g_event_watch_installed = false;
/** The system rounds the window's corners (Windows 11, RequestSystemRoundedCorners): our outline follows them. */
bool g_system_rounds_corners = false;
/** Whether the dark theme is active: picks the colour of the macOS window's outline (see SetFrameDark). */
bool g_frame_dark = false;

float DpToWindow(SDL_Window* window, float dp) {
  if (!window || dp <= 0.f) {
    return 0.f;
  }
  const float display_scale = SDL_GetWindowDisplayScale(window);
  const float density = SDL_GetWindowPixelDensity(window);
  const float scale = (density > 0.f) ? (display_scale / density) : display_scale;
  return dp * (scale > 0.f ? scale : 1.f);
}

bool PointInTitlebarDrag(SDL_Window* window, int x, int y) {
  int win_w = 0;
  int win_h = 0;
  if (!SDL_GetWindowSize(window, &win_w, &win_h) || win_w <= 0) {
    return false;
  }
  (void)win_h;
  if (y < 0 || y >= g_layout.titlebar_height_win) {
    return false;
  }
  if (x < 0 || x >= win_w) {
    return false;
  }
  if (g_layout.controls_leading) {
    return x >= g_layout.controls_width_win;
  }
  return x < win_w - g_layout.controls_width_win;
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
/** Quiet time after the last resize / scale / expose event before the macOS frame is settled (shadow, outline width). */
constexpr Uint32 kFrameSettleDelayMs = 200;
SDL_TimerID g_frame_settle_timer = 0;

void ApplyMacFrame(SDL_Window* window) {
  const bool square = DesktopWindowChrome::IsMaximized() || ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
  desktop::ApplyMacWindowRoundedCorners(window, square, g_frame_dark);
}

// `userdata` is the id of the timer that queued this run. A newer ScheduleFrameSettle may have replaced the global by
// now: clearing it then would lose the newer handle, which a reschedule or Uninstall could no longer cancel.
void SDLCALL FrameSettleOnMainThread(void* userdata) {
  if (g_frame_settle_timer == static_cast<SDL_TimerID>(reinterpret_cast<uintptr_t>(userdata))) {
    g_frame_settle_timer = 0;
  }
  if (SDL_Window* window = Backend::GetWindow()) {
    ApplyMacFrame(window);
  }
}

Uint32 SDLCALL FrameSettleTimer(void* /*userdata*/, SDL_TimerID id, Uint32 /*interval*/) {
  SDL_RunOnMainThread(FrameSettleOnMainThread, reinterpret_cast<void*>(static_cast<uintptr_t>(id)),
                      false); // the timer runs on its own thread; AppKit needs main
  return 0;
}

/** (Re)start the debounce: the shadow and the outline are re-applied once, after the window has stopped changing. */
void ScheduleFrameSettle() {
  if (g_frame_settle_timer != 0) {
    SDL_RemoveTimer(g_frame_settle_timer);
  }
  g_frame_settle_timer = SDL_AddTimer(kFrameSettleDelayMs, FrameSettleTimer, nullptr);
}
#endif

bool SDLCALL TitlebarEventWatch(void* /*userdata*/, SDL_Event* event) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
  if (event) {
    switch (event->type) {
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_EXPOSED: // the first frame is on screen
      case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (SDL_Window* window = Backend::GetWindow(); window && event->window.windowID == SDL_GetWindowID(window)) {
          ScheduleFrameSettle();
        }
        break;
      default:
        break;
    }
  }
#endif
  if (!event || event->type != SDL_EVENT_MOUSE_BUTTON_DOWN) {
    return true;
  }
  if (event->button.button != SDL_BUTTON_LEFT || event->button.clicks != 2) {
    return true;
  }
  SDL_Window* window = Backend::GetWindow();
  if (!window || event->button.windowID != SDL_GetWindowID(window)) {
    return true;
  }
  if (PointInTitlebarDrag(window, static_cast<int>(event->button.x), static_cast<int>(event->button.y))) {
    DesktopWindowChrome::ToggleMaximize();
  }
  return true;
}

SDL_HitTestResult SDLCALL HitTestCallback(SDL_Window* window, const SDL_Point* area, void* /*data*/) {
  if (!window || !area) {
    return SDL_HITTEST_NORMAL;
  }

  int win_w = 0;
  int win_h = 0;
  if (!SDL_GetWindowSize(window, &win_w, &win_h) || win_w <= 0 || win_h <= 0) {
    return SDL_HITTEST_NORMAL;
  }

  const int x = area->x;
  const int y = area->y;
  const int edge = std::max(1, g_layout.edge_margin_win);
  const bool maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;

  if (!maximized) {
    const bool left = x < edge;
    const bool right = x >= win_w - edge;
    const bool top = y < edge;
    const bool bottom = y >= win_h - edge;
    if (top && left) {
      return SDL_HITTEST_RESIZE_TOPLEFT;
    }
    if (top && right) {
      return SDL_HITTEST_RESIZE_TOPRIGHT;
    }
    if (bottom && left) {
      return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    }
    if (bottom && right) {
      return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    }
    if (left) {
      return SDL_HITTEST_RESIZE_LEFT;
    }
    if (right) {
      return SDL_HITTEST_RESIZE_RIGHT;
    }
    if (top) {
      return SDL_HITTEST_RESIZE_TOP;
    }
    if (bottom) {
      return SDL_HITTEST_RESIZE_BOTTOM;
    }
  }

  if (PointInTitlebarDrag(window, x, y)) {
    return SDL_HITTEST_DRAGGABLE;
  }

  return SDL_HITTEST_NORMAL;
}
#endif

} // namespace

bool DesktopWindowChrome::Enabled() {
  return Platform::IsDesktop();
}

bool DesktopWindowChrome::ControlsLeading() {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
  return Platform::IsDesktop();
#else
  return false;
#endif
}

void DesktopWindowChrome::Install() {
#if UI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return;
  }
  SDL_Window* window = Backend::GetWindow();
  if (!window) {
    return;
  }
  SDL_SetWindowHitTest(window, HitTestCallback, nullptr);
  if (!g_event_watch_installed) {
    SDL_AddEventWatch(TitlebarEventWatch, nullptr);
    g_event_watch_installed = true;
  }
  RefreshAppearance();
#if defined(__APPLE__) && !TARGET_OS_IPHONE
  ScheduleFrameSettle(); // once more after the first frame has been presented
#endif
#endif
}

void DesktopWindowChrome::SetFrameDark(bool dark) {
#if UI_SDL_VERSION_MAJOR >= 3
  g_frame_dark = dark;
#if defined(__APPLE__) && !TARGET_OS_IPHONE
  if (Enabled()) {
    if (SDL_Window* window = Backend::GetWindow()) {
      ApplyMacFrame(window);
    }
  }
#endif
#else
  (void)dark;
#endif
}

void DesktopWindowChrome::RefreshAppearance() {
#if UI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return;
  }
  SDL_Window* window = Backend::GetWindow();
  if (!window) {
    return;
  }
  // Windows 11 rounds the corners itself (square again while maximized); elsewhere this asks nothing.
  g_system_rounds_corners = desktop::RequestSystemRoundedCorners(window);
#if defined(__APPLE__) && !TARGET_OS_IPHONE
  ApplyMacFrame(window);
#endif
#endif
}

bool DesktopWindowChrome::SystemRoundsCorners() {
#if UI_SDL_VERSION_MAJOR >= 3
  return g_system_rounds_corners;
#else
  return false;
#endif
}

void DesktopWindowChrome::Uninstall() {
#if UI_SDL_VERSION_MAJOR >= 3
#if defined(__APPLE__) && !TARGET_OS_IPHONE
  if (g_frame_settle_timer != 0) {
    SDL_RemoveTimer(g_frame_settle_timer);
    g_frame_settle_timer = 0;
  }
#endif
  SDL_Window* window = Backend::GetWindow();
  if (window) {
    SDL_SetWindowHitTest(window, nullptr, nullptr);
  }
  if (g_event_watch_installed) {
    SDL_RemoveEventWatch(TitlebarEventWatch, nullptr);
    g_event_watch_installed = false;
  }
#endif
}

void DesktopWindowChrome::SetLayout(float titlebar_height_dp, float controls_width_dp, float edge_margin_dp,
                                    bool controls_leading) {
#if UI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return;
  }
  SDL_Window* window = Backend::GetWindow();
  if (!window) {
    return;
  }
  g_layout.titlebar_height_win = static_cast<int>(std::lround(DpToWindow(window, titlebar_height_dp)));
  g_layout.controls_width_win = static_cast<int>(std::lround(DpToWindow(window, controls_width_dp)));
  g_layout.edge_margin_win = static_cast<int>(std::lround(DpToWindow(window, edge_margin_dp)));
  g_layout.controls_leading = controls_leading;
  g_layout.titlebar_height_win = std::max(1, g_layout.titlebar_height_win);
  g_layout.controls_width_win = std::max(0, g_layout.controls_width_win);
  g_layout.edge_margin_win = std::max(1, g_layout.edge_margin_win);
#else
  (void)titlebar_height_dp;
  (void)controls_width_dp;
  (void)edge_margin_dp;
  (void)controls_leading;
#endif
}

void DesktopWindowChrome::Minimize() {
#if UI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return;
  }
  if (SDL_Window* window = Backend::GetWindow()) {
    SDL_MinimizeWindow(window);
  }
#endif
}

void DesktopWindowChrome::ToggleMaximize() {
#if UI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return;
  }
  SDL_Window* window = Backend::GetWindow();
  if (!window) {
    return;
  }
  if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) {
    SDL_RestoreWindow(window);
  } else {
    SDL_MaximizeWindow(window);
  }
  RefreshAppearance();
#endif
}

void DesktopWindowChrome::Close() {
  if (!Enabled()) {
    return;
  }
  Backend::RequestExit();
}

bool DesktopWindowChrome::IsMaximized() {
#if UI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return false;
  }
  SDL_Window* window = Backend::GetWindow();
  return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
#else
  return false;
#endif
}

#if UI_SDL_VERSION_MAJOR >= 3 && !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
// GNOME X11 ignores XDG_ACTIVATION_TOKEN (SDL only consumes it on Wayland).
// Apply the notification ActivationToken as _NET_STARTUP_ID and activate via EWMH.
bool RaiseX11WithStartupId(SDL_Window* window, const std::string& token) {
  if (!window || SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") != 0) {
    return false;
  }
  const SDL_PropertiesID props = SDL_GetWindowProperties(window);
  auto* display = static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  const Window xid = static_cast<Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!display || xid == 0) {
    return false;
  }

  if (!token.empty()) {
    const Atom startup_id = XInternAtom(display, "_NET_STARTUP_ID", False);
    XChangeProperty(display, xid, startup_id, XA_STRING, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(token.c_str()), static_cast<int>(token.size()));
  }

  XMapRaised(display, xid);
  XRaiseWindow(display, xid);

  const Atom net_active = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
  XEvent ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = xid;
  ev.xclient.message_type = net_active;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = 1; // application
  ev.xclient.data.l[1] = CurrentTime;
  ev.xclient.data.l[2] = 0;
  XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  XFlush(display);
  return true;
}
#endif

void DesktopWindowChrome::RaiseAndFocus() {
#if UI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return;
  }
  SDL_Window* window = Backend::GetWindow();
  if (!window) {
    logging::getLogger("LocalNotifier").warning << "RaiseAndFocus: no window";
    return;
  }

  // Do not SDL_FlashWindow here: on X11/GNOME urgency becomes the intermediate
  // "App is ready" banner that forces a second click.
  (void)SDL_FlashWindow(window, SDL_FLASH_CANCEL);

  // SDL Wayland RaiseWindow consumes XDG_ACTIVATION_TOKEN; GNOME Notifications
  // emit ActivationToken (startup id / xdg-activation) before ActionInvoked.
  const std::string token = desktop::TakePendingDesktopActivationToken();
  logging::getLogger("LocalNotifier").info << "RaiseAndFocus token=" << (token.empty() ? "none" : "yes");

  if (!token.empty()) {
    SDL_setenv_unsafe("XDG_ACTIVATION_TOKEN", token.c_str(), 1);
    SDL_setenv_unsafe("DESKTOP_STARTUP_ID", token.c_str(), 1);
  }

  SDL_ShowWindow(window);
  if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
    SDL_RestoreWindow(window);
  }

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
  const bool x11_raised = RaiseX11WithStartupId(window, token);
#else
  const bool x11_raised = false;
#endif
  if (!x11_raised) {
    SDL_RaiseWindow(window);
  }

  if (!token.empty()) {
    SDL_unsetenv_unsafe("XDG_ACTIVATION_TOKEN");
    SDL_unsetenv_unsafe("DESKTOP_STARTUP_ID");
  }
#endif
}

} // namespace pbr
