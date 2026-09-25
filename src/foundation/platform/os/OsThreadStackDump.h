#pragma once

namespace pbr::os {

/**
 * Best-effort dump of every thread's backtrace to stderr (Linux).
 * Used by lab probes when teardown hangs; no-op on other platforms.
 */
void DumpAllThreadStacks();

} // namespace pbr::os
