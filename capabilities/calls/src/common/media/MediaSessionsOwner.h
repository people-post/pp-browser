#pragma once

#include "common/runtime/AppRuntime.h"

#include "common/PbrCompat.h"

namespace pbr {

/**
 * The owner thread calls run their sessions on, and broadcast on calls' engine (projects/thread-ownership T001).
 * Calls names it; the app registers it with AppRuntime (MediaSessionsOwnerThread in its owner list).
 */
inline constexpr OwnerThreadId kMediaSessionsOwner{0};

/** The owner and its OS thread name (`pp-media-sess`; a metrics label, NODE_METRICS.md). */
inline pp::OwnerThreadSpec MediaSessionsOwnerThread() {
  return {kMediaSessionsOwner, "pp-media-sess"};
}

/** Tests: a runtime with this owner, Dedicated. */
inline AppRuntimeConfig MediaSessionsRuntimeConfig() {
  AppRuntimeConfig config;
  config.owners = {MediaSessionsOwnerThread()};
  return config;
}

/** Tests: a runtime with this owner, Manual (drained by the test). */
inline AppRuntimeConfig MediaSessionsManualRuntimeConfig() {
  return pp::ManualOwnerRuntimeConfig({MediaSessionsOwnerThread()});
}

} // namespace pbr
