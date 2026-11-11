#pragma once

namespace pbr::os {

/**
 * Developer builds' switches set from a computer: Android reads the system property `name`
 * (`adb shell setprop <name> 1`; debug.* properties need no root) and says whether it is "1". False everywhere
 * else. Callers ask only under kDevToolsBuild. Any thread.
 */
bool DebugFlagSet(const char* name);

} // namespace pbr::os
