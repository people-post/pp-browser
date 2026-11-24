#pragma once

#include "common/MeshConfig.h"
#include "common/ServiceEndpoints.h"
#include "feature/conversations/HubPolicyPrefs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/**
 * What the conversations hub reads of the app's settings, as values (capability-repos CR015): the app projects
 * them from its config file and the profile's preferences (feature/product HubSettingsFor) and hands them over.
 */
struct HubSettings {
  /** The mesh slice, normalized (the app's seeds, else the deployment's). */
  MeshConfig mesh;
  ServiceEndpointConfig relay;
  /** Our other relays' base URLs (`relay.extra_base_urls`, multi-relay R2), after `relay`'s; read at start. */
  std::vector<std::string> relay_extra_base_urls;
  DirectoryConfig directory;
  ServiceEndpointConfig registration;
  /**
   * The backend table's other services (profile-model PM002), by their own addresses: **empty is absent** (what needs it is
   * off; no other service's address stands in). Account lockers, client compatibility, the Brief LLM token service.
   */
  std::string account_locker_url;
  std::string client_compat_url;
  std::string llm_token_url;
  /** Dogfood: the initiation floor seeded into the local identity while it is still 0 (P001). */
  int64_t initiation_floor = 0;
  /** The app's data directory (machine-level files: the mesh directory cache). */
  std::string app_data_dir;
  /**
   * The backend this profile is the home profile of (profile-model PM009): what the device keeps of it lives in the
   * profile's `backends/{backend_id}/` (registration.enc, account_locker.json). Empty: kept at the profile's top level.
   */
  std::string backend_id;
  /** The profile's policies as they stand at start (later changes arrive through Apply). */
  HubPolicyPrefs policy;
  /** The system's name for this device, read once at startup on the main thread (empty: unknown). */
  std::string system_device_name;
  /**
   * A developer build: a relay a person adds or a QR code names may be a plain `http://` one or not on a public
   * host (IsPublicRelayOrigin; the lab's). Release builds: neither.
   */
  bool developer_relays = false;
  /**
   * The org's relay (and registration URL) may be pinned on its first valid certificate when the build or config names
   * no relay id for it (multi-relay MR008 item 4): a developer build, or the compiled prod / staging / dev backend of a
   * build that ships none (dogfood, warned). Off — a release build's custom backend or PP_BROWSER_ORIGIN without `relay.relay_id`
   * (`registration.relay_id`): that relay is not used.
   */
  bool org_first_use = false;
  /**
   * The build names a placeholder for the org relay's id (DeploymentProfile kPendingRelayId: its operator set is not
   * made yet): it fails closed — the org's relay is not used and never pinned on first use, in any build.
   */
  bool org_relay_pending = false;
  /** The build's certificate for the org's relay (standard base64; "" none): the floor its pins start from (Q10). */
  std::string org_relay_certificate_b64;
};

} // namespace pbr
