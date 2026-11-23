#pragma once

#include "foundation/data/Config.h"
#include "foundation/platform/DeploymentProfile.h"

#include <string>
#include <string_view>

namespace pbr {

/** Trimmed, scheme and host lower-cased, no trailing `/`: the same backend always yields the same text. */
std::string NormalizeBackendUrl(const std::string& url);

/**
 * A custom backend's profile key: `custom-<12 hex>` in a store build, `test-custom-<12 hex>` in a tester build (the same
 * FNV-1a 64 of the normalized `url`), so a tester build never opens a store user's custom profile.
 */
std::string CustomProfileKey(const std::string& url, BuildKind kind);

/**
 * The profile key of backend `env` in a build with `compiled` (`registration_url` names a custom one). `env` is first
 * resolved as the latch does (ResolveLatchedBackend), so a tester build never yields `prod`: `prod`, `staging`, `dev`,
 * CustomProfileKey.
 */
std::string BackendProfileKeyFor(BackendEnv env, const std::string& registration_url, const CompiledOrigins& compiled);

/**
 * Whether a build of `kind` may open a profile whose manifest records `recorded_backend` (ReadProfileBackend; "" for
 * none): a store build any; a tester build none of a store build's — `prod`, `custom-…` — so the store app's data on
 * the same device is never opened or changed by it (release-compatibility RC022).
 */
bool ProfileBackendAllowed(std::string_view recorded_backend, BuildKind kind);

/**
 * Filesystem-safe key naming the backend this process runs against, also the profile id created for it:
 * BackendProfileKeyFor the running backend, or CustomProfileKey of the origin when PP_BROWSER_ORIGIN is set.
 */
std::string BackendProfileKey(const AppConfig& config);

/** The key of the backend a saved config selects: differs from the running one once a switch awaits restart. */
std::string SavedBackendKey(const AppConfig& config);

} // namespace pbr
