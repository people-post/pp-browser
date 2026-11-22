#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace pbr {

inline constexpr const char* kProductDirName = "pp-browser";

/** Which backend the user picked (config `backend.env`). `Custom` uses the URLs saved in the config file. */
enum class BackendEnv { Prod, Staging, Dev, Custom };

/** The former name `qa` parses as `Staging` (release-compatibility RC009 as amended); unknown names parse as `Prod`. */
BackendEnv ParseBackendEnv(std::string_view name);
const char* BackendEnvName(BackendEnv env);

/**
 * Backend origin (scheme://host, no path, no trailing slash). Precedence: PP_BROWSER_ORIGIN, then the
 * latched backend's compiled origin (`Staging` -> CMake PP_API_URL_STAGING, `Dev` -> PP_API_URL_DEV, otherwise
 * PP_API_URL_PROD); none set yields "". Read on every call. All URLs derived from it are empty when the origin is empty, which
 * disables the backend features.
 */
std::string BriefOrigin();
/**
 * A compiled relay id that stands for an operator set not made yet (release-compatibility CUTOVER_PLAN: Leo makes the
 * dev, staging and prod sets and their certificate 1): the build's default for each until then. It **fails closed**: it
 * is no relay id, so the org relay of that environment is not used (nothing is signed for it, and it is never pinned
 * on first use); release checks refuse it (`scripts/check/check_relay_ids.sh`).
 */
inline constexpr char kPendingRelayId[] = "pending";
/** `relay_id` is the placeholder (kPendingRelayId). */
inline bool IsPendingRelayId(std::string_view relay_id) {
  return relay_id == kPendingRelayId;
}

/**
 * The compile-time origins (CMake PP_API_URL_PROD / _STAGING / _DEV), their relays' ids (PP_RELAY_ID_PROD / _STAGING /
 * _DEV: `relay:…`, kPendingRelayId, or "" for none — multi-relay MR008 as SIGNED_FORMATS § 3.3 amends it: the
 * org relay's address ships with the build) and each relay's newest certificate at build time, the floor a fresh
 * install starts from (`config/relay-certs/<env>.cert`, standard base64; "" for none: § 3.5, Q10).
 */
struct CompiledOrigins {
  const char* prod = "";
  const char* staging = "";
  const char* dev = "";
  const char* prod_relay_id = "";
  const char* staging_relay_id = "";
  const char* dev_relay_id = "";
  const char* prod_relay_certificate = "";
  const char* staging_relay_certificate = "";
  const char* dev_relay_certificate = "";
};
/** `compiled`'s origin for `env` (`Custom` has none: ""). */
const char* CompiledOriginFor(BackendEnv env, const CompiledOrigins& compiled);
/** Whether `env` can run: `Prod` and `Custom` always, `Staging` / `Dev` only with a compiled origin. */
bool BackendAvailableIn(BackendEnv env, const CompiledOrigins& compiled);
/** This build's compile-time origins. */
CompiledOrigins BuildCompiledOrigins();
/**
 * The rule behind BriefOrigin(), with its inputs passed in. The UI demo app (`ui_demo`) has no backend: always "".
 * Otherwise `env_origin` (PP_BROWSER_ORIGIN, may be null) wins over `compiled`'s origin for `backend`.
 */
std::string ResolveBriefOrigin(bool ui_demo, const char* env_origin, BackendEnv backend,
                               const CompiledOrigins& compiled);
/** The org relay's identity as the build (or PP_BROWSER_ORIGIN's PP_BROWSER_RELAY_ID) names it. */
struct OrgRelayIdentity {
  /** `relay:…`; kPendingRelayId (the environment's operator set is not made yet: fails closed); "" for none. */
  std::string relay_id;
  /** The build's certificate for it (standard base64): the floor; "" for none (fetched from the relay). */
  std::string certificate_b64;
};
/**
 * The relay id the org's relay (BriefRelayBaseUrl) must prove (MR008, SIGNED_FORMATS § 3.5), and the build's
 * certificate for it: `compiled`'s for `backend` (`Staging` / `Dev` → their own, otherwise prod's); none in the UI demo
 * app. When
 * `env_origin` (PP_BROWSER_ORIGIN) replaces the compiled origin, `env_relay_id` (PP_BROWSER_RELAY_ID, may be null) and
 * no certificate: none there, a developer build pins that relay on its first valid certificate (warned) and a release
 * build does not use it.
 */
OrgRelayIdentity ResolveOrgRelayIdentity(bool ui_demo, const char* env_origin, BackendEnv backend,
                                         const CompiledOrigins& compiled, const char* env_relay_id = nullptr);
/**
 * Whether the org's relay may be pinned on its first valid certificate when no relay id is named for it (multi-relay
 * MR008 item 4): in a developer build, or on a compiled backend (`compiled_backend`: prod / staging / dev, no PP_BROWSER_ORIGIN) —
 * a build that names no relay id is a dogfood one (release workflows refuse it). A custom backend or
 * PP_BROWSER_ORIGIN in a release build names its relay's id, or that relay is not used. A pending relay id
 * (kPendingRelayId) is never taken on first use: the hub refuses it before asking this.
 */
inline bool OrgRelayFirstUseAllowed(bool developer_build, bool compiled_backend) {
  return developer_build || compiled_backend;
}
/** ResolveOrgRelayIdentity for this process (the latched backend). */
OrgRelayIdentity BriefOrgRelayIdentity();
/** BriefOrgRelayIdentity's relay id. */
std::string BriefRelayId();
/** The compile-time prod relay's id ("" unless configured; pp-node's default with the compiled URL). */
const char* CompiledDefaultRelayId();
/** The compile-time prod relay's certificate ("" unless configured; pp-node's floor with the compiled URL). */
const char* CompiledDefaultRelayCertificate();
/** False in the UI demo app: nothing may leave the device (crash reports, run logs). */
inline bool UploadAllowed(bool ui_demo) {
  return !ui_demo;
}
/** The "simulated replies, no LLM configured" banner: hidden in the UI demo app (it has no backend by design). */
inline bool MockReplyBannerVisible(bool ui_demo) {
  return !ui_demo;
}
/** A direct chat's link line and "can't reach" banner: hidden in the UI demo app (offline by design, so every chat
 *  would say "can't connect" and no screen would match the mockups). */
inline bool PeerLinkStatusVisible(bool ui_demo) {
  return !ui_demo;
}
/**
 * The one gate of "Collect run logs" / "Send run logs" (DEV/PLAN-send-run-logs.md, a test-phase feature): false hides
 * both rows and keeps only this launch's and the previous launch's logs, whatever the stored switch says. To switch
 * the feature off in a build, make this false.
 */
inline bool RunLogsAllowed(bool ui_demo) {
  return UploadAllowed(ui_demo);
}
/** Runs `sender` only when UploadAllowed(ui_demo); returns whether it ran. */
bool RunIfUploadAllowed(bool ui_demo, const std::function<void()>& sender);
/** The Me -> Network environment switch (prod / staging / dev / custom): hidden in the UI demo app (its profile key stays `prod`). */
inline bool BackendSwitchVisible(bool ui_demo) {
  return !ui_demo;
}
/** Me -> Security "change PIN": hidden in the UI demo app, whose bootstrap PIN is fixed (a change would lock it out). */
inline bool ChangePinVisible(bool ui_demo) {
  return !ui_demo;
}
/** The compile-time prod origin ("" unless configured). */
const char* CompiledDefaultOrigin();
/** The compile-time staging origin ("" unless configured); empty hides the Staging option. */
const char* CompiledStagingOrigin();
/** The compile-time dev origin ("" unless configured); empty hides the Dev option. */
const char* CompiledDevOrigin();
/** The compile-time default of `relay.content_sha256` (CMake PP_RELAY_CONTENT_SHA256; false unless set). */
bool CompiledRelayContentSha256();
/** BackendAvailableIn for this build. */
bool BackendAvailable(BackendEnv env);
/** The rule behind LatchActiveBackend(): `Staging` / `Dev` without a compiled origin in `compiled` are `Prod`. */
BackendEnv ResolveLatchedBackend(BackendEnv requested, const CompiledOrigins& compiled);

/**
 * The backend this process runs against. The first call to `LatchActiveBackend` wins, so a settings
 * change takes effect on the next start only. `Staging` / `Dev` without a compiled origin latch as `Prod`.
 */
BackendEnv ActiveBackend();
void LatchActiveBackend(BackendEnv requested);
bool ActiveBackendLatched();
/** Tests only. */
void ResetActiveBackendForTest();
/** True when PP_BROWSER_ORIGIN is set and non-empty. */
bool OriginEnvOverrideActive();

std::string BriefLlmBaseUrl();
/** The backend URLs derived from an `origin` (as BriefOrigin() gives it); each "" when the origin is "". */
std::string BriefLlmBaseUrlFor(const std::string& origin);
std::string BriefRelayBaseUrlFor(const std::string& origin);
std::string BriefMcpUrlFor(const std::string& origin);
/** Dev override for the brief_AI stream endpoint (PP_BROWSER_BRIEF_STREAM_URL); empty = derive from the LLM base URL. */
std::string BriefStreamUrlOverride();
/** The rule behind BriefStreamUrlOverride(), inputs passed in: always "" in the UI demo app (no backend, no redirect). */
std::string ResolveBriefStreamUrlOverride(bool ui_demo, const char* env_url);
std::string BriefRelayBaseUrl();
std::string BriefMcpUrl();

/** Config/data/cache root basename. */
const char* ProductDirBasename();

} // namespace pbr
