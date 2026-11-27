// Linux (X11 / Wayland) through SDL: file managers offer the copied files as "text/uri-list".
#include "foundation/platform/ClipboardFiles.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>

namespace pbr {
namespace {

// D2 kill switch for Linux: set to false to leave a file manager's copy to the text field (D1 behaviour) on this platform only.
constexpr bool kFilePasteEnabled = true;

constexpr const char* kUriList = "text/uri-list";

} // namespace

bool ClipboardFilePasteEnabled() {
  return kFilePasteEnabled;
}

bool ClipboardHasFiles() {
  size_t count = 0;
  char** list = SDL_GetClipboardMimeTypes(&count);
  if (list == nullptr) {
    return false;
  }
  bool offered = false;
  for (size_t i = 0; i < count && !offered; ++i) {
    offered = list[i] != nullptr && std::string_view(list[i]) == kUriList;
  }
  SDL_free(list);
  // Only a uri-list that names at least one local file counts (a stale or remote-only list is not "files"): the list's
  // text is read only when the type is offered.
  return offered && !ReadClipboardFiles().empty();
}

std::vector<std::string> ReadClipboardFiles() {
  size_t size = 0;
  void* raw = SDL_GetClipboardData(kUriList, &size);
  if (raw == nullptr) {
    return {};
  }
  const std::string body(static_cast<const char*>(raw), size);
  SDL_free(raw);
  return ParseUriListFiles(body);
}

} // namespace pbr
