#pragma once

#include "lib/people/ContactTypes.h"
#include "lib/people/ProfileIconCache.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

Roe<void> FetchProfileIcon(const std::string& profile_dir, const std::string& cache_key, const mesh::ProfileIconRef& icon);

Roe<void> FetchProfileIconIfNeeded(const std::string& profile_dir, const std::string& cache_key,
                                   const mesh::ProfileIconRef& icon);

} // namespace pbr
