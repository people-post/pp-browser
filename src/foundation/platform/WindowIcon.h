#ifndef PP_BROWSER_WINDOW_ICON_H
#define PP_BROWSER_WINDOW_ICON_H

#include <string>

struct SDL_Window;

namespace pbr {

bool SetWindowIconFromAsset(SDL_Window* window, const std::string& relative_asset_path);

} // namespace pbr

#endif // PP_BROWSER_WINDOW_ICON_H
