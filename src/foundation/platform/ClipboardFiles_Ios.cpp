// Ios: no desktop file paste. Always "no files"; the kill switch is off.
#include "foundation/platform/ClipboardFiles.h"

namespace pbr {

bool ClipboardFilePasteEnabled() {
  return false;
}

bool ClipboardHasFiles() {
  return false;
}

std::vector<std::string> ReadClipboardFiles() {
  return {};
}

} // namespace pbr
