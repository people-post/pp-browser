#include "foundation/platform/NativeFileDialog.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <memory>
#include <utility>

namespace pbr {

namespace {

struct DialogState {
  NativeFileDialogCallback callback;
};

void OnDialogComplete(void* userdata, const char* const* filelist, int /*filter*/) {
  std::unique_ptr<DialogState> state(static_cast<DialogState*>(userdata));
  if (!state || !state->callback) {
    return;
  }
  std::vector<std::string> paths;
  if (filelist && filelist[0]) {
    for (const char* const* cursor = filelist; *cursor != nullptr; ++cursor) {
      paths.emplace_back(*cursor);
    }
  }
  state->callback(std::move(paths));
}

} // namespace

void ShowOpenImageFileDialog(SDL_Window* window, NativeFileDialogCallback callback) {
  if (!callback) {
    return;
  }
  static const SDL_DialogFileFilter filters[] = {
      {"Images", "png;jpg;jpeg;webp;gif"},
      {"All files", "*"},
  };
  auto* state = new DialogState{.callback = std::move(callback)};
  SDL_ShowOpenFileDialog(OnDialogComplete, state, window, filters, 2, nullptr, false);
}

void ShowOpenFileDialog(SDL_Window* window, NativeFileDialogCallback callback) {
  if (!callback) {
    return;
  }
  auto* state = new DialogState{.callback = std::move(callback)};
  SDL_ShowOpenFileDialog(OnDialogComplete, state, window, nullptr, 0, nullptr, false);
}

} // namespace pbr
