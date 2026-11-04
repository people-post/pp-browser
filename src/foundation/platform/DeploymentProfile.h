#pragma once

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
/** The compile-time prod origin ("" unless configured). */
const char* CompiledDefaultOrigin();
/** The compile-time QA origin ("" unless configured); empty hides the QA option. */
const char* CompiledQaOrigin();
inline bool QaBackendAvailable() { return *CompiledQaOrigin() != '\0'; }

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
/** Dev override for the brief_AI stream endpoint (PP_BROWSER_BRIEF_STREAM_URL); empty = derive from the LLM base URL. */
std::string BriefStreamUrlOverride();
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
