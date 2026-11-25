// Windows 11: the DWM rounds a window's corners when asked (DWMWA_WINDOW_CORNER_PREFERENCE). Our window is borderless
// (an SDL popup-style window), which the system leaves square unless asked. dwmapi is loaded at run time, so nothing new
// is linked; Windows 10 refuses the attribute and the window stays square.

#include "foundation/platform/desktop/WindowCorners.h"

#include <SDL3/SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace pbr {
namespace desktop {
namespace {

// dwmapi.h's values, written out: an older SDK has no names for them.
constexpr DWORD kDwmwaWindowCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE (Windows 11 build 22000+)
constexpr DWORD kDwmwcpRound = 2;                  // DWMWCP_ROUND: the system's small rounded corner

using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

DwmSetWindowAttributeFn DwmSetWindowAttributeEntry() {
  static const DwmSetWindowAttributeFn entry = []() -> DwmSetWindowAttributeFn {
    HMODULE module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<DwmSetWindowAttributeFn>(
                        reinterpret_cast<void*>(GetProcAddress(module, "DwmSetWindowAttribute")))
                  : nullptr;
  }();
  return entry;
}

} // namespace

bool RequestSystemRoundedCorners(SDL_Window* window) {
  if (window == nullptr) {
    return false;
  }
  auto* hwnd = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  const DwmSetWindowAttributeFn set_attribute = DwmSetWindowAttributeEntry();
  if (hwnd == nullptr || set_attribute == nullptr) {
    return false;
  }
  const DWORD preference = kDwmwcpRound; // the attribute's value is a DWORD-sized enum
  return SUCCEEDED(
      set_attribute(hwnd, kDwmwaWindowCornerPreference, &preference, static_cast<DWORD>(sizeof(preference))));
}

} // namespace desktop
} // namespace pbr
