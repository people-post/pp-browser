// SDL3 file dialog: desktop and Android. iOS has its own (NativeFileDialog_Ios.mm).
#include "foundation/platform/NativeFileDialog.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include "foundation/platform/CameraCapture.h"
#include "foundation/platform/CapturePathRules.h"
#include "foundation/platform/PickedFileName.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

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

void ShowOpenPhotoLibraryDialog(SDL_Window* window, NativeFileDialogCallback callback) {
  if (!callback) {
    return;
  }
  static const SDL_DialogFileFilter filters[] = {
      {"Photos and videos", "png;jpg;jpeg;webp;gif;mp4;mov;m4v;webm;3gp"},
      {"All files", "*"},
  };
  auto* state = new DialogState{.callback = std::move(callback)};
  SDL_ShowOpenFileDialog(OnDialogComplete, state, window, filters, 2, nullptr, false);
}

// The dialog returns the user's own file; only what the camera wrote under CaptureScratchDir() (Android's cache
// `capture/`; empty on the desktop, so nothing there) is ours to delete.
void ReleasePickedFile(const std::string& path) {
  if (!IsInsideScratchDir(path, CaptureScratchDir())) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

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

std::optional<ImportedPick> ImportPickedFile(const std::string& picked, const std::string& scratch_dir,
                                             const bool force_copy) {
  const bool is_uri = picked.rfind("content://", 0) == 0;
  ImportedPick pick;
  pick.name = DisplayNameFromPickedPath(picked);
  if (!is_uri && !force_copy) {
    pick.path = picked;
    if (pick.name.empty()) {
      pick.name = std::filesystem::path(picked).filename().string();
    }
    return pick;
  }
  if (scratch_dir.empty()) {
    return std::nullopt;
  }
  SDL_IOStream* in = SDL_IOFromFile(picked.c_str(), "rb");
  if (in == nullptr) {
    return std::nullopt;
  }
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::path(scratch_dir) /
                                    (std::to_string(SDL_GetTicksNS()) + "-" + std::to_string(reinterpret_cast<uintptr_t>(in)));
  std::filesystem::create_directories(dir, ec);
  const std::string file_name = pick.name.empty() ? std::string("attachment") : pick.name;
  const std::filesystem::path dest = dir / file_name;
  std::FILE* out = ec ? nullptr : std::fopen(dest.string().c_str(), "wb");
  bool ok = out != nullptr;
  if (ok) {
    std::vector<char> buffer(1 << 20);
    while (true) {
      const size_t n = SDL_ReadIO(in, buffer.data(), buffer.size());
      if (n == 0) {
        break;
      }
      if (std::fwrite(buffer.data(), 1, n, out) != n) {
        ok = false;
        break;
      }
    }
    ok = (std::fclose(out) == 0) && ok;
  }
  SDL_CloseIO(in);
  if (!ok) {
    std::filesystem::remove_all(dir, ec);
    return std::nullopt;
  }
  pick.path = dest.string();
  pick.copied = true;
  return pick;
}

void RemoveImportedPick(const ImportedPick& pick) {
  if (!pick.copied || pick.path.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(pick.path).parent_path(), ec);
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
