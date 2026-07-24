#pragma once

#include <string>

namespace pbr {

/** Open an https/http URL in the platform browser / handler. Returns false on failure. */
bool PlatformOpenUrl(const std::string& url);

} // namespace pbr
