#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

struct ProfileIconCacheMeta {
  std::string url;
  std::string blob_id;
  std::string kind;
  int64_t fetched_at_ms = 0;
  std::string local_filename;
};

std::string ProfileIconCacheRoot(const std::string& profile_dir);

Roe<ProfileIconCacheMeta> LoadProfileIconCacheMeta(const std::string& profile_dir);

/** Absolute path to cached icon file, or empty when none. */
std::string ProfileIconLocalPath(const std::string& profile_dir);

Roe<void> SaveProfileIconCache(const std::string& profile_dir, const std::vector<uint8_t>& bytes,
                               const std::string& file_extension, const ProfileIconCacheMeta& meta);

Roe<void> ClearProfileIconCache(const std::string& profile_dir);

} // namespace pbr
