#pragma once

#include "common/PbrCompat.h"

#include <string>

namespace pbr::os {

/**
 * Tags a file as "came from another person over the network" so the OS and its default handlers
 * apply their download protections (macOS quarantine xattr, Windows Zone.Identifier stream).
 * No-op on platforms without such a mark. Best effort: callers log and carry on if it fails.
 */
Roe<void> MarkFileAsDownloaded(const std::string& path);

} // namespace pbr::os
