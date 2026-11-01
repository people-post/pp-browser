#pragma once

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** The profile directory's `manifest.json` (docs/contracts/DATA_LAYOUT.md): written when missing, else checked and migrated. */
Roe<void> EnsureProfileManifest(const std::string& profile_data_dir);

} // namespace pbr
