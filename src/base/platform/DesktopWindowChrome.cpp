#include "base/platform/DesktopWindowChrome.h"

#include "base/platform/Platform.h"

#include "RmlUi_Backend.h"

#include <algorithm>
#include <cmath>

#if RMLUI_SDL_VERSION_MAJOR >= 3
#include <SDL3/SDL.h>
#endif

namespace pbr {
namespace {

#if RMLUI_SDL_VERSION_MAJOR >= 3
struct HitTestLayout {
  int titlebar_height_win = 36;
  int controls_width_win = 120;
  int edge_margin_win = 5;
};

HitTestLayout g_layout{};
bool g_event_watch_installed = false;

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
  return x >= 0 && x < win_w - g_layout.controls_width_win;
}

bool SDLCALL TitlebarEventWatch(void* /*userdata*/, SDL_Event* event) {
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
  if (PointInTitlebarDrag(window, static_cast<int>(event->button.x),
                          static_cast<int>(event->button.y))) {
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

void DesktopWindowChrome::Install() {
#if RMLUI_SDL_VERSION_MAJOR >= 3
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
#endif
}

void DesktopWindowChrome::Uninstall() {
#if RMLUI_SDL_VERSION_MAJOR >= 3
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

void DesktopWindowChrome::SetLayout(float titlebar_height_dp, float controls_width_dp,
                                    float edge_margin_dp) {
#if RMLUI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return;
  }
  SDL_Window* window = Backend::GetWindow();
  if (!window) {
    return;
  }
  g_layout.titlebar_height_win =
      static_cast<int>(std::lround(DpToWindow(window, titlebar_height_dp)));
  g_layout.controls_width_win =
      static_cast<int>(std::lround(DpToWindow(window, controls_width_dp)));
  g_layout.edge_margin_win =
      static_cast<int>(std::lround(DpToWindow(window, edge_margin_dp)));
  g_layout.titlebar_height_win = std::max(1, g_layout.titlebar_height_win);
  g_layout.controls_width_win = std::max(0, g_layout.controls_width_win);
  g_layout.edge_margin_win = std::max(1, g_layout.edge_margin_win);
#else
  (void)titlebar_height_dp;
  (void)controls_width_dp;
  (void)edge_margin_dp;
#endif
}

void DesktopWindowChrome::Minimize() {
#if RMLUI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return;
  }
  if (SDL_Window* window = Backend::GetWindow()) {
    SDL_MinimizeWindow(window);
  }
#endif
}

void DesktopWindowChrome::ToggleMaximize() {
#if RMLUI_SDL_VERSION_MAJOR >= 3
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
#endif
}

void DesktopWindowChrome::Close() {
  if (!Enabled()) {
    return;
  }
  Backend::RequestExit();
}

bool DesktopWindowChrome::IsMaximized() {
#if RMLUI_SDL_VERSION_MAJOR >= 3
  if (!Enabled()) {
    return false;
  }
  SDL_Window* window = Backend::GetWindow();
  return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
#else
  return false;
#endif
}

} // namespace pbr
