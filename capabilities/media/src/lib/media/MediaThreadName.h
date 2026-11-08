#pragma once

namespace pbr {

/** Names the calling thread (Apple: shows in ReadThreadCpu samples and debuggers; elsewhere: no-op). */
void SetMediaThreadName(const char* name);

} // namespace pbr
