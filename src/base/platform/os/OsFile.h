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

} // namespace pbr::os
