#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "common/PathUtf8.h"
#include "foundation/platform/ClipboardFiles.h"

#include <filesystem>
#include <string>
#include <vector>

namespace pbr {
namespace {

// D2 kill switch for Windows: set to false to leave an Explorer copy to the text field (D1 behaviour) on this platform only.
constexpr bool kFilePasteEnabled = true;

/** The clipboard open for reading; closed on every way out. Another program may hold it a moment: 3 tries, 10 ms apart (as SDL does). */
class ClipboardGuard {
public:
  ClipboardGuard() {
    for (int attempt = 0; attempt < 3 && !open_; ++attempt) {
      open_ = OpenClipboard(nullptr) != FALSE;
      if (!open_ && attempt < 2) {
        Sleep(10);
      }
    }
  }
  ~ClipboardGuard() {
    if (open_) {
      CloseClipboard();
    }
  }
  ClipboardGuard(const ClipboardGuard&) = delete;
  ClipboardGuard& operator=(const ClipboardGuard&) = delete;
  bool Open() const { return open_; }

private:
  bool open_ = false;
};

} // namespace

bool ClipboardImageQueryIsCheap() {
  return true;
}

bool ClipboardFilePasteEnabled() {
  return kFilePasteEnabled;
}

bool ClipboardHasFiles() {
  return IsClipboardFormatAvailable(CF_HDROP) != FALSE;
}

std::vector<std::string> ReadClipboardFiles() {
  std::vector<std::string> files;
  ClipboardGuard clipboard;
  if (!clipboard.Open()) {
    return files;
  }
  HANDLE data = GetClipboardData(CF_HDROP);
  if (data == nullptr) {
    return files;
  }
  HDROP drop = static_cast<HDROP>(data);
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
  for (UINT i = 0; i < count; ++i) {
    const UINT length = DragQueryFileW(drop, i, nullptr, 0); // characters, without the terminator
    if (length == 0) {
      continue;
    }
    std::wstring path(static_cast<size_t>(length) + 1, L'\0');
    const UINT copied = DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size()));
    if (copied == 0) {
      continue;
    }
    path.resize(static_cast<size_t>(copied));
    files.push_back(PathToUtf8(std::filesystem::path(path)));
  }
  return files;
}

} // namespace pbr
