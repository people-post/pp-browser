#include "base/platform/WindowIcon.h"

#include "base/platform/IAssetLocator.h"

#include <SDL3_image/SDL_image.h>
#include <SDL3/SDL.h>

namespace pbr {

bool SetWindowIconFromAsset(SDL_Window* window, const std::string& relative_asset_path) {
  if (window == nullptr) {
    return false;
  }

  const std::string path = IAssetLocator::Instance().Resolve(relative_asset_path);
  SDL_Surface* surface = IMG_Load(path.c_str());
  if (surface == nullptr) {
    return false;
  }

  const bool ok = SDL_SetWindowIcon(window, surface);
  SDL_DestroySurface(surface);
  return ok;
}

} // namespace pbr
