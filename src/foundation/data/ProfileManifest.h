#pragma once

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The profile directory's `manifest.json` (docs/contracts/DATA_LAYOUT.md): written when missing, else checked and
 * migrated. A new manifest records `backend_key` (BackendProfileKey) when it is non-empty.
 */
Roe<void> EnsureProfileManifest(const std::string& profile_data_dir, const std::string& backend_key = {});

/** The backend the profile was created or adopted for; empty when none is recorded (or no manifest). */
std::string ReadProfileBackend(const std::string& profile_data_dir);

/** Records `backend_key` in an existing manifest that has none; a recorded backend is never replaced. */
Roe<void> AdoptProfileBackend(const std::string& profile_data_dir, const std::string& backend_key);

} // namespace pbr
