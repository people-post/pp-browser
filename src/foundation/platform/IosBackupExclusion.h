#pragma once

#include <string>

namespace pbr {

// iOS only: marks `dir` (and so everything under it) as excluded from iCloud and iTunes backups (PB-040).
// Message history and keys stay on the device. Returns false when the attribute could not be set.
bool ExcludeDirFromIosBackup(const std::string& dir);

} // namespace pbr
