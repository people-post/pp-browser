#pragma once

#include <string>

namespace pbr::os {

/**
 * The name the file manager shows for the folder at `path` (UTF-8): on macOS Finder's localized name ("Downloads"
 * reads "下载" in Chinese); elsewhere the folder's own name. Empty only for an empty path.
 */
std::string FolderDisplayName(const std::string& path);

} // namespace pbr::os
