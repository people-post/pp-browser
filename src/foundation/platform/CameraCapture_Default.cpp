#include "foundation/platform/CameraCapture.h"

namespace pbr {

// Desktop: no camera entry in the composer, so this is only reached by mistake; say so rather than hang.
void ShowCameraCapture(SDL_Window* /*window*/, CameraCaptureKind /*kind*/, int /*max_video_seconds*/,
                       uint64_t /*max_video_bytes*/, CameraCaptureCallback callback) {
  if (callback) {
    callback(CameraCaptureOutcome::Unavailable, {});
  }
}

std::string CaptureScratchDir() {
  return {};
}

} // namespace pbr
