// SDL3 file dialog: desktop and Android. iOS has its own (NativeFileDialog_Ios.mm).
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

void ShowOpenImageFileDialog(SDL_Window* window, NativeFileDialogCallback callback, const bool include_heic) {
  if (!callback) {
    return;
  }
  static const SDL_DialogFileFilter filters[] = {
      {"Images", "png;jpg;jpeg;webp;gif"},
      {"All files", "*"},
  };
  static const SDL_DialogFileFilter filters_heic[] = {
      {"Images", "png;jpg;jpeg;webp;gif;heic;heif"},
      {"All files", "*"},
  };
  auto* state = new DialogState{.callback = std::move(callback)};
  SDL_ShowOpenFileDialog(OnDialogComplete, state, window, include_heic ? filters_heic : filters, 2, nullptr, false);
}

void ReleasePickedFile(const std::string& /*path*/) {} // the dialog returns the user's own file

void ShowOpenImageDocumentDialog(SDL_Window* window, NativeFileDialogCallback callback, const bool include_heic) {
  ShowOpenImageFileDialog(window, std::move(callback), include_heic);
}

void ShowOpenFileDialog(SDL_Window* window, NativeFileDialogCallback callback) {
  if (!callback) {
    return;
  }
  auto* state = new DialogState{.callback = std::move(callback)};
  SDL_ShowOpenFileDialog(OnDialogComplete, state, window, nullptr, 0, nullptr, false);
}

namespace {

/** SDL_IOFromFile takes paths and Android content:// URIs alike. */
bool WriteWhole(const std::string& path, const std::string& bytes) {
  SDL_IOStream* out = SDL_IOFromFile(path.c_str(), "wb");
  if (out == nullptr) {
    return false;
  }
  const bool written = SDL_WriteIO(out, bytes.data(), bytes.size()) == bytes.size();
  return SDL_CloseIO(out) && written;
}

} // namespace

void SaveFileAs(SDL_Window* window, const std::string& suggested_name, std::string bytes,
                std::function<void(bool saved)> done) {
  if (!done) {
    return;
  }
  auto* state = new DialogState{.callback = [bytes = std::move(bytes), done](std::vector<std::string> paths) {
    done(!paths.empty() && WriteWhole(paths.front(), bytes));
  }};
  SDL_ShowSaveFileDialog(OnDialogComplete, state, window, nullptr, 0, suggested_name.c_str());
}

std::optional<std::string> ReadPickedFile(const std::string& path, const size_t max_bytes) {
  size_t size = 0;
  void* data = SDL_LoadFile(path.c_str(), &size);
  if (data == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string> out;
  if (size <= max_bytes) {
    out.emplace(static_cast<const char*>(data), size);
  }
  SDL_free(data);
  return out;
}

} // namespace pbr
