#pragma once

#include "foundation/platform/DeploymentProfile.h"

namespace pbr {

/**
 * B233: the CallKit / audio-session timing lines in the developer log are written wherever the QA origin is compiled
 * in. Today that is every iOS build, the TestFlight one that becomes the store release included (ios-testflight.yml
 * always passes PP_API_URL_QA); this is not a store-build gate and is not meant as one. The lines are timestamps and
 * flags of our own call provider only: no phone numbers, no UUIDs, nothing about any other app. They leave the device
 * only through the user's own "send logs" action, with log collection switched on. Revisit when the store build drops
 * the QA origin (RC015).
 */
inline bool CallKitDiagnosticsEnabled(const char* compiled_qa_origin) {
  return compiled_qa_origin != nullptr && *compiled_qa_origin != '\0';
}

inline bool CallKitDiagnosticsEnabled() {
  return CallKitDiagnosticsEnabled(CompiledQaOrigin());
}

} // namespace pbr
