#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace pbr {

/** What the system camera is opened for. iOS offers both in one screen; Android has one intent per kind. */
enum class CameraCaptureKind { Photo, Video, PhotoOrVideo };

/** How a capture ended. The GUI maps Unavailable / Unprocessable to a user message; Cancelled says nothing. */
enum class CameraCaptureOutcome {
  Picked,         // `paths` holds the one captured file
  Cancelled,      // the user backed out of the camera
  Unavailable,    // no camera, camera permission denied, no camera app, or the camera returned no file
  Unprocessable,  // the file came back but could not be made safe to send (metadata strip failed)
};

using CameraCaptureCallback = std::function<void(CameraCaptureOutcome outcome, std::vector<std::string> paths)>;

/**
 * Open the system camera and hand back one file, ready for the attachment send path. Phones only; the desktop
 * implementation answers Unavailable at once (the GUI never shows the entry there). The file is written under
 * CaptureScratchDir() so ReleasePickedFile() can delete it; pictures carry no EXIF, videos have their metadata
 * (location included) stripped without re-encoding and keep their orientation. A video is limited to
 * `max_video_seconds` and `max_video_bytes` as far as the platform camera honours them; the caller checks the size
 * again. The callback may run on any thread.
 */
void ShowCameraCapture(SDL_Window* window, CameraCaptureKind kind, int max_video_seconds, uint64_t max_video_bytes,
                       CameraCaptureCallback callback);

/**
 * The directory captured files live in: Android the app's cache `capture/`, iOS `tmp/pp-picks` (shared with the
 * photo picker), desktop empty — nothing is ever deleted there by ReleasePickedFile on the strength of its name.
 */
std::string CaptureScratchDir();

} // namespace pbr
