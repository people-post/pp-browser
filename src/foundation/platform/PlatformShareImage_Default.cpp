#include "foundation/platform/PlatformShareImage.h"

namespace pbr {

bool PlatformCanShareImage() {
  return false;
}

bool PlatformShareImage(SDL_Window* /*window*/, const std::vector<uint8_t>& /*png*/, ShareAnchor /*anchor*/) {
  return false;
}

ImageSaveKind PlatformImageSaveKind() {
  return ImageSaveKind::None;
}

void PlatformSaveImage(SDL_Window* /*window*/, const std::string& /*suggested_name*/, std::vector<uint8_t> /*png*/,
                       std::function<void(ImageSaveResult)> done) {
  if (done) {
    done(ImageSaveResult::Failed);
  }
}

} // namespace pbr
