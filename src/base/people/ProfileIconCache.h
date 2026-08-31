#pragma once

#include "base/people/ContactTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct ProfileIconCacheMeta {
  std::string url;
  std::string blob_id;
  std::string kind;
  int64_t fetched_at_ms = 0;
  std::string local_filename;
};

/** Filesystem-safe cache directory segment (relay/account ids may contain `:`). */
std::string SanitizeProfileIconCacheKey(const std::string& key);

std::string ProfileIconCacheKeyForHit(const DirectoryHit& hit);
std::string ProfileIconCacheKeyForContact(const Contact& contact);

std::string ProfileIconCacheRoot(const std::string& profile_dir, const std::string& cache_key = "self");

Roe<ProfileIconCacheMeta> LoadProfileIconCacheMeta(const std::string& profile_dir,
                                                   const std::string& cache_key = "self");

/** Absolute path to cached icon file, or empty when none. */
std::string ProfileIconLocalPath(const std::string& profile_dir, const std::string& cache_key = "self");

Roe<void> SaveProfileIconCache(const std::string& profile_dir, const std::vector<uint8_t>& bytes,
                               const std::string& file_extension, const ProfileIconCacheMeta& meta,
                               const std::string& cache_key = "self");

Roe<void> ClearProfileIconCache(const std::string& profile_dir, const std::string& cache_key = "self");

} // namespace pbr
