#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace pbr {

inline constexpr const char* kProductDirName = "pp-browser";

/** Which backend the user picked (config `backend.env`). `Custom` uses the URLs saved in the config file. */
enum class BackendEnv { Prod, Qa, Custom };

/** Unknown names parse as `Prod`. */
BackendEnv ParseBackendEnv(std::string_view name);
const char* BackendEnvName(BackendEnv env);

/**
 * Backend origin (scheme://host, no path, no trailing slash). Precedence: PP_BROWSER_ORIGIN, then the
 * latched backend's compiled origin (`Qa` -> CMake PP_API_URL_QA, otherwise PP_API_URL_PROD); none set
 * yields "". Read on every call. All URLs derived from it are empty when the origin is empty, which
 * disables the backend features.
 */
std::string BriefOrigin();
/**
 * The compile-time origins (CMake PP_API_URL_PROD / PP_API_URL_QA) and their relays' PeerIds (PP_RELAY_PEER_ID_PROD /
 * PP_RELAY_PEER_ID_QA, multi-relay MR008: the org relay's address ships with the build); "" where not configured.
 */
struct CompiledOrigins {
  const char* prod = "";
  const char* qa = "";
  const char* prod_relay_peer_id = "";
  const char* qa_relay_peer_id = "";
};
/** This build's compile-time origins. */
CompiledOrigins BuildCompiledOrigins();
/**
 * The rule behind BriefOrigin(), with its inputs passed in. The UI demo app (`ui_demo`) has no backend: always "".
 * Otherwise `env_origin` (PP_BROWSER_ORIGIN, may be null) wins over `compiled`'s origin for `backend`.
 */
std::string ResolveBriefOrigin(bool ui_demo, const char* env_origin, BackendEnv backend,
                               const CompiledOrigins& compiled);
/**
 * The PeerId the org's relay (BriefRelayBaseUrl) must answer to (MR008): `compiled`'s for `backend` (`Qa` → the QA
 * one, otherwise prod's); "" in the UI demo app. When `env_origin` (PP_BROWSER_ORIGIN) replaces the compiled origin,
 * `env_peer_id` (PP_BROWSER_RELAY_PEER_ID, may be null): none there, a developer build pins that relay on its first
 * answer (warned) and a release build does not use it.
 */
std::string ResolveRelayPeerId(bool ui_demo, const char* env_origin, BackendEnv backend,
                               const CompiledOrigins& compiled, const char* env_peer_id = nullptr);
/**
 * Whether the org's relay may be pinned on its first identity answer when none is named for it (multi-relay MR008
 * item 4): in a developer build, or on a compiled backend (`compiled_backend`: prod / qa, no PP_BROWSER_ORIGIN) — a
 * build that ships no PeerId is a dogfood one (release workflows refuse it). A custom backend or PP_BROWSER_ORIGIN in a
 * release build names its relay's PeerId, or that relay is not used.
 */
inline bool OrgRelayFirstUseAllowed(bool developer_build, bool compiled_backend) {
  return developer_build || compiled_backend;
}
/** ResolveRelayPeerId for this process (the latched backend). */
std::string BriefRelayPeerId();
/** The compile-time prod relay's PeerId ("" unless configured; pp-node's default with the compiled URL). */
const char* CompiledDefaultRelayPeerId();
/**
 * The rule behind DiagnosticsUploadUrl(): "" in the UI demo app; else `env_url` (PP_BROWSER_DIAGNOSTICS_URL, may be
 * null) when set, else `origin` + /api/support/diagnostics ("" when origin is empty).
 */
std::string ResolveDiagnosticsUploadUrl(bool ui_demo, const char* env_url, const std::string& origin);
/** False in the UI demo app: nothing may leave the device (diagnostics, crash reports). */
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
/** Runs `sender` only when UploadAllowed(ui_demo); returns whether it ran. */
bool RunIfUploadAllowed(bool ui_demo, const std::function<void()>& sender);
/** The Me -> Network prod / qa / custom switch: hidden in the UI demo app (its profile key stays `prod`). */
inline bool BackendSwitchVisible(bool ui_demo) {
  return !ui_demo;
}
/** Me -> Security "change PIN": hidden in the UI demo app, whose bootstrap PIN is fixed (a change would lock it out). */
inline bool ChangePinVisible(bool ui_demo) {
  return !ui_demo;
}
/** The compile-time prod origin ("" unless configured). */
const char* CompiledDefaultOrigin();
/** The compile-time QA origin ("" unless configured); empty hides the QA option. */
const char* CompiledQaOrigin();
/** The compile-time default of `relay.content_sha256` (CMake PP_RELAY_CONTENT_SHA256; false unless set). */
bool CompiledRelayContentSha256();
inline bool QaBackendAvailable() {
  return *CompiledQaOrigin() != '\0';
}
/** The rule behind LatchActiveBackend(): `Qa` without a QA origin (`qa_available` false) is `Prod`. */
BackendEnv ResolveLatchedBackend(BackendEnv requested, bool qa_available);

/**
 * The backend this process runs against. The first call to `LatchActiveBackend` wins, so a settings
 * change takes effect on the next start only. `Qa` without a compiled QA origin latches as `Prod`.
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
/**
 * Where "Send diagnostics" posts: BriefOrigin() + /api/support/diagnostics. PP_BROWSER_DIAGNOSTICS_URL (a full
 * URL) replaces it for local development against DEV/tools/fake_diagnostics.py only; it is not remote config.
 * Empty when neither is set.
 */
std::string DiagnosticsUploadUrl();
std::string BriefRelayBaseUrl();
std::string BriefMcpUrl();

/** Config/data/cache root basename. */
const char* ProductDirBasename();

} // namespace pbr
