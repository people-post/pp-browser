#pragma once

#include "domain/people/ContactTypes.h"
#include "domain/people/ProfileIconCache.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

bool ProfileIconNeedsFetch(const std::string& profile_dir, const std::string& cache_key,
                           const ProfileIconRef& icon);

Roe<void> FetchProfileIcon(const std::string& profile_dir, const std::string& cache_key, const ProfileIconRef& icon);

Roe<void> FetchProfileIconIfNeeded(const std::string& profile_dir, const std::string& cache_key,
                                   const ProfileIconRef& icon);

} // namespace pbr
