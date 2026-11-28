#pragma once

#include <string>

namespace pbr::os {

/** Best-effort OS name for the calling thread (debuggers, TSan reports, `top -H`). Truncated as the OS requires. */
void SetCurrentThreadName(const std::string& name);

} // namespace pbr::os
