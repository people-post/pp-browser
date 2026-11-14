#pragma once

#include "foundation/platform/PlatformShare.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/** Whether this platform has a system sheet that takes a picture (iOS, macOS); the UI shows its Share button by it. */
bool PlatformCanShareImage();

/**
 * Share the PNG `png` (handed over from memory, never written to a temp file) through the system sheet anchored to
 * `anchor` inside `window`. False when there is no sheet or it could not be shown. UI thread only.
 */
bool PlatformShareImage(SDL_Window* window, const std::vector<uint8_t>& png, ShareAnchor anchor = {});

/** Where PlatformSaveImage puts a picture: the photo library (iOS), a file the user picks (macOS), nowhere. */
enum class ImageSaveKind { None, Photos, File };
ImageSaveKind PlatformImageSaveKind();

enum class ImageSaveResult {
  Saved,
  /** The user closed the save dialog (a file save); nothing to say. */
  Cancelled,
  /** The user has not allowed adding to the photo library. */
  Denied,
  Failed,
};

/**
 * Save the PNG `png` (`suggested_name` is the file name a save dialog offers) as PlatformImageSaveKind says; `done`
 * runs exactly once, on any thread. With ImageSaveKind::None it runs at once with Failed.
 */
void PlatformSaveImage(SDL_Window* window, const std::string& suggested_name, std::vector<uint8_t> png,
                       std::function<void(ImageSaveResult)> done);

} // namespace pbr
