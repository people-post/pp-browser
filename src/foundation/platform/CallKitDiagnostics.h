#pragma once

#include "foundation/platform/DeploymentProfile.h"

namespace pbr {

/**
 * B233: the CallKit / audio-session timing lines in the developer log are written wherever the staging origin is compiled
 * in. Today that is every iOS build, the TestFlight one that becomes the store release included (ios-testflight.yml
 * always passes PP_API_URL_STAGING); this is not a store-build gate and is not meant as one. The lines are timestamps and
 * flags of our own call provider only: no phone numbers, no UUIDs, nothing about any other app. They leave the device
 * only through the user's own "send logs" action, with log collection switched on. Revisit when the store build drops
 * the staging origin (RC015).
 */
inline bool CallKitDiagnosticsEnabled(const char* compiled_staging_origin) {
  return compiled_staging_origin != nullptr && *compiled_staging_origin != '\0';
}

inline bool CallKitDiagnosticsEnabled() {
  return CallKitDiagnosticsEnabled(CompiledStagingOrigin());
}

} // namespace pbr
