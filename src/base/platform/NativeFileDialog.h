#pragma once

#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace pbr {

using NativeFileDialogCallback = std::function<void(std::vector<std::string> paths)>;

/** Async native open dialog for a single image file. Empty paths = cancel. */
void ShowOpenImageFileDialog(SDL_Window* window, NativeFileDialogCallback callback);
void ShowOpenFileDialog(SDL_Window* window, NativeFileDialogCallback callback);

} // namespace pbr
