#pragma once

#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace pbr {

using NativeFileDialogCallback = std::function<void(std::vector<std::string> paths)>;

/**
 * Async native open dialog for a single image file. Empty paths = cancel. `include_heic` adds heic/heif to the
 * filter. The callback may run on any thread. On iOS the photo picker is used and the path is a private copy
 * under tmp/pp-picks/ (cleared on the next pick).
 */
void ShowOpenImageFileDialog(SDL_Window* window, NativeFileDialogCallback callback, bool include_heic = false);
void ShowOpenFileDialog(SDL_Window* window, NativeFileDialogCallback callback);

} // namespace pbr
