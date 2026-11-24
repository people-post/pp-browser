#pragma once

#include "foundation/data/Config.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"

#include <string>
#include <string_view>

namespace pbr {

/** Trimmed, scheme and host lower-cased, no trailing `/`: the same backend always yields the same text. */
std::string NormalizeBackendUrl(const std::string& url);

/**
 * A custom backend's id by profile-model PM010: `custom-<12 hex>` (`test-custom-<12 hex>` in a tester build) over the
 * normalized registration URL, the org relay's normalized origin and its relay id — and nothing else, so editing the
 * locker, client-compat, LLM, crash intake, directory or MCP address keeps the id. The prefix names the network: a
 * tester build's never names a store build's profile (release-compatibility RC022). The id names the backend's folder
 * under a profile's `backends/` (profile-model PM009).
 */
std::string CustomBackendId(const std::string& registration_url, const std::string& org_relay_url,
                            const std::string& org_relay_id, BuildKind kind);
/** `table.id` for a built-in table, CustomBackendId of a custom one. */
std::string BackendTableId(const BackendTable& table, BuildKind kind);

/**
 * The id of backend `env` in a build with `compiled` (a custom one named by its registration URL and org relay). `env` is
 * first resolved as the latch does (ResolveLatchedBackend), so a tester build never yields `prod`: `prod`, `staging`,
 * `dev`, CustomBackendId.
 */
std::string BackendIdFor(BackendEnv env, const std::string& registration_url, const std::string& org_relay_url,
                         const std::string& org_relay_id, const CompiledOrigins& compiled);

/**
 * The network a backend id's profiles are on (profile-model PM001): `prod` and a store build's `custom-…` on `main`,
 * `staging`, `dev`, a tester build's `test-custom-…` on `test-custom`, `node` (pp-node) on `node`; empty for anything
 * else.
 */
std::string_view NetworkOfBackend(std::string_view backend_id);

/**
 * Whether a build of `kind` may open a profile on `network`: a store build `main`, `staging` and `dev`; a tester build
 * `staging`, `dev` and `test-custom`, never `main`, so the store app's data on the same device is never opened or
 * changed by it (release-compatibility RC022, profile-model PM001). `node` is pp-node's alone.
 */
bool ProfileNetworkAllowed(std::string_view network, BuildKind kind);

/**
 * The id of the backend this process runs against, the folder of its home profile's `backends/` (profile-model PM009):
 * BackendIdFor the running backend, or the PP_BROWSER_ORIGIN table's id when that is set.
 */
std::string BackendProfileKey(const AppConfig& config);

/** The id of the backend a saved config selects: differs from the running one once a switch awaits restart. */
std::string SavedBackendKey(const AppConfig& config);

/**
 * A backend switch awaits a restart (profile-model P4, #747): the saved config selects another backend id than the run's
 * (`running_key`, BootstrapResult::backend_key) — another environment, or a custom backend's registration URL or org
 * relay edited (PM010). The one rule for the restart banner and for push. No running key known: the environments.
 */
bool BackendSwitchPending(const AppConfig& saved, const std::string& running_key);

} // namespace pbr
