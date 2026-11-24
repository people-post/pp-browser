#pragma once

#include "foundation/platform/DeploymentProfile.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * Which accounts a backend's profiles may meet (profile-model PM001): `main` is prod and the store app's custom
 * backends; `test-custom` is a tester app's custom backends.
 */
enum class BackendNetwork { Main, Staging, Dev, TestCustom };
const char* BackendNetworkName(BackendNetwork network);

/**
 * The services a custom backend's table names, as the config file gives them (profile-model PM002, PM011). Every field
 * is optional: a service the user did not give is **absent** and what needs it is off — it never falls back to a
 * built-in backend's address. Only the keys of `backend.services` live in the config file's new optional object
 * (`config_version` is unchanged); the org relay, registration and directory are the existing `relay`, `registration`
 * and `directory` keys.
 */
struct CustomServices {
  /** The org's relay: `https://host/api/relay` and the relay id (`relay:…`) it must prove; "" names none (fails closed). */
  std::string org_relay_url;
  std::string org_relay_id;
  /** The registration URL and its relay id. */
  std::string registration_url;
  std::string registration_relay_id;
  /** The directory providers, the first one primary, the rest failing over (`directory.providers`, else `base_url`). */
  std::vector<std::string> directory_providers;
  /** The rest: base URLs, "" for absent. The LLM ones are `…/api/llm/v1`-style bases (the renew path is appended). */
  std::string account_locker_url;
  std::string client_compat_url;
  std::string llm_gateway_url;
  std::string llm_token_url;
  std::string promoted_mcp_url;
  std::string crash_intake_url;
  std::string mesh_nodes_url;
  /** The extra relays a new profile's list starts with (`relay.extra_base_urls`). */
  std::vector<std::string> seed_relays;

  bool operator==(const CustomServices& other) const;
  bool operator!=(const CustomServices& other) const { return !(*this == other); }
  /** Nothing named: the table of a custom backend before anything is typed in. */
  bool Empty() const { return *this == CustomServices{}; }
};

/**
 * A backend: an id, a network and one address per service (profile-model PM002). The built-in tables (`prod`,
 * `staging`, `dev`) are compiled from the build's origins (CompiledOrigins, filled in from the one origin each);
 * a custom one is the user's (CustomServices). **A service with an empty address is absent: whatever needs it is off and
 * nothing is sent to another backend.** The relay ids ride with the relay-shaped services.
 */
struct BackendTable {
  /** `prod`, `staging`, `dev`; a custom table is `custom` / `test-custom` (its PM010 id is CustomBackendId). */
  std::string id;
  BackendNetwork network = BackendNetwork::Main;
  /** The org's relay, the relay id it must prove ("" none; kPendingRelayId fails closed), the build's certificate. */
  std::string org_relay_url;
  std::string org_relay_id;
  std::string org_relay_certificate_b64;
  std::string registration_url;
  std::string registration_relay_id;
  /** The directory: the first is primary, more fail over. Empty: no directory. */
  std::vector<std::string> directory_providers;
  /** Where lockers are kept (a base URL; `/v1/lockers/…` is appended). */
  std::string account_locker_url;
  /** `GET <this>/v1/client-compat`: support, relay certificate, switches. */
  std::string client_compat_url;
  /** The Brief LLM gateway, `…/api/llm/v1` (the brief preset's base URL). */
  std::string llm_gateway_url;
  /** The Brief LLM token service, `…/api/llm/v1` (`/token/renew` is appended). */
  std::string llm_token_url;
  /** The promoted MCP the profile starts with (a seed: the user's own value wins, PM011). */
  std::string promoted_mcp_url;
  /** Where crash reports go (a base URL; `/v1/crash-reports` is appended unless the compat document names its own). */
  std::string crash_intake_url;
  /** The mesh node list source (a seed; the directory's `/v1/mesh/nodes`). */
  std::string mesh_nodes_url;
  /** The extra relays a new profile's relay list starts with (a seed). */
  std::vector<std::string> seed_relays;

  /** The primary directory's address ("" none). */
  std::string DirectoryUrl() const { return directory_providers.empty() ? std::string() : directory_providers.front(); }
};

/** The table of built-in backend `env` (`Prod` / `Staging` / `Dev`) in a build with `compiled`; `Custom` yields an empty custom table. */
BackendTable BuiltInBackendTable(BackendEnv env, const CompiledOrigins& compiled);
/**
 * The table PP_BROWSER_ORIGIN names: every service under that one origin (a developer's explicit choice), the org relay's
 * id as PP_BROWSER_RELAY_ID gives it. Empty origin: an empty table. It is a custom backend of `kind`'s network.
 */
BackendTable OriginBackendTable(const std::string& origin, const std::string& relay_id, BuildKind kind);
/** A custom backend's table: exactly what `services` names, nothing taken from anywhere else. */
BackendTable CustomBackendTable(const CustomServices& services, BuildKind kind);
/**
 * The rule behind ActiveBackendTable(), inputs passed in: the UI demo app has no backend (empty table); PP_BROWSER_ORIGIN
 * (`env_origin`) names an origin table; `Custom` is `custom`'s table; anything else the built-in table.
 */
BackendTable ResolveBackendTable(bool ui_demo, const char* env_origin, const char* env_relay_id, BackendEnv backend,
                                 const CompiledOrigins& compiled, const CustomServices& custom);

/** The table of the backend this process runs against (the latch, PP_BROWSER_ORIGIN, the custom services latched with it). */
BackendTable ActiveBackendTable();
/** The table a config with nothing saved starts from: ActiveBackendTable(), but a custom backend starts empty. */
BackendTable DefaultBackendTable();

} // namespace pbr
