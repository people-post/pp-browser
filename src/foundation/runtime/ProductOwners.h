#pragma once

#include "common/runtime/AppRuntime.h"

#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * pp-browser's owner threads besides UI and Mesh I/O (projects/thread-ownership T001), registered with
 * pp-cpp-common's AppRuntime. Mesh control runs on the mesh's own io lane (THREADING.md § Mesh threads).
 */
inline constexpr OwnerThreadId kMediaSessionsOwner{0};

/** The product's owners and their OS thread names (`pp-media-sess`; a metrics label, NODE_METRICS.md). */
std::vector<pp::OwnerThreadSpec> ProductOwnerThreads();

/** The runtime config every pp-browser process starts from: the product's owners, Dedicated. */
AppRuntimeConfig ProductRuntimeConfig();

/** Tests and harnesses: the product's owners in Manual mode (drained by the test). */
AppRuntimeConfig ManualOwnerRuntimeConfig();

} // namespace pbr
