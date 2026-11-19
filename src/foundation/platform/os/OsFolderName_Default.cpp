#include "foundation/platform/os/OsFolderName.h"

#include "common/PathUtf8.h"

namespace pbr::os {

// No localized folder names here: the folder's own name.
std::string FolderDisplayName(const std::string& path) {
  const std::filesystem::path folder = PathFromUtf8(path);
  const std::filesystem::path name = folder.filename().empty() ? folder.parent_path().filename() : folder.filename();
  return name.empty() ? path : PathToUtf8(name);
}

} // namespace pbr::os
