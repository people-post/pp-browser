#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace pbr {

/**
 * True when `path` is a file below `scratch_dir` after both are resolved (so `scratch_dir/../x` is not). An empty
 * `scratch_dir` (the desktop has no capture directory) matches nothing: a user's own file is never released on the
 * strength of its name.
 */
inline bool IsInsideScratchDir(const std::string& path, const std::string& scratch_dir) {
  if (scratch_dir.empty() || path.empty()) {
    return false;
  }
  std::error_code ec;
  const std::filesystem::path root = std::filesystem::weakly_canonical(scratch_dir, ec);
  if (ec) {
    return false;
  }
  const std::filesystem::path file = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    return false;
  }
  const std::filesystem::path relative = file.lexically_relative(root);
  if (relative.empty() || relative == ".") {
    return false;
  }
  return *relative.begin() != "..";
}

} // namespace pbr
