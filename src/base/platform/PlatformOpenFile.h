#pragma once

#include <string>

namespace pbr {

/** Open a local file with the OS default handler (R012). */
bool PlatformOpenFile(const std::string& path);

} // namespace pbr
