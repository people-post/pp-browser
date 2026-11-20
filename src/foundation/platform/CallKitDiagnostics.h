#pragma once

#include "foundation/platform/DeploymentProfile.h"

namespace pbr {

/**
 * B233: the CallKit / audio-session timing lines in the developer log exist only in builds that can reach the QA
 * backend (TestFlight, our own installs) — the store build compiles no QA origin, so it writes none of them. They are
 * observations: timestamps and flags, never a number, identifier or anything of another app's.
 */
inline bool CallKitDiagnosticsEnabled(const char* compiled_qa_origin) {
  return compiled_qa_origin != nullptr && *compiled_qa_origin != '\0';
}

inline bool CallKitDiagnosticsEnabled() {
  return CallKitDiagnosticsEnabled(CompiledQaOrigin());
}

} // namespace pbr
