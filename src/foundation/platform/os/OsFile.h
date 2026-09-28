#pragma once

#include "common/Error.h"

#include <cstdint>
#include <filesystem>
#include "common/PbrCompat.h"

namespace pbr::os {

uint64_t GetPid();

Roe<void> FsyncFile(const std::filesystem::path& path);
void FsyncDirectory(const std::filesystem::path& dir);
Roe<void> AtomicRename(const std::filesystem::path& tmp_path, const std::filesystem::path& final_path);

/**
 * Restrict a just-written file to owner read/write (POSIX 0600). Config/preferences files can
 * hold secrets (e.g. `api_key` — see ConfigJson.cpp); best-effort (Windows: no-op — ACLs are not
 * modeled here, and the per-user profile directory is the existing boundary).
 */
void SetOwnerOnlyPermissions(const std::filesystem::path& path);

} // namespace pbr::os
