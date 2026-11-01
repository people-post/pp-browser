#pragma once

#include <string>

namespace pbr {

inline constexpr const char* kProductionBriefOrigin = "https://www.brief.global";
inline constexpr const char* kSandboxBriefOrigin = "https://www-en.qa.peoplepost.org";
inline constexpr const char* kProductDirName = "pp-browser";
inline constexpr const char* kSandboxProductDirName = "pp-browser-sandbox";

/** Runtime-only; set from --sandbox or PP_BROWSER_SANDBOX before config load. */
void SetSandboxMode(bool enabled);
bool SandboxMode();

const char* BriefOrigin();
std::string BriefLlmBaseUrl();
/** Dev override for the brief_AI stream endpoint (PP_BROWSER_BRIEF_STREAM_URL); empty = derive from the LLM base URL. */
std::string BriefStreamUrlOverride();
/**
 * Where "Send diagnostics" posts: BriefOrigin() + /api/support/diagnostics. PP_BROWSER_DIAGNOSTICS_URL (a full
 * URL) replaces it for local development against DEV/tools/fake_diagnostics.py only; it is not remote config.
 */
std::string DiagnosticsUploadUrl();
std::string BriefRelayBaseUrl();
std::string BriefMcpUrl();

/** pp-browser vs pp-browser-sandbox (config/data/cache roots). */
const char* ProductDirBasename();

/** When sandbox is on, rewrite production Brief URLs to the sandbox origin. */
std::string RewriteBriefOriginUrl(std::string url);

/** Parse --sandbox from argv; falls back to PP_BROWSER_SANDBOX when unset. */
bool ResolveSandboxModeFromLaunch(int argc, char** argv);

} // namespace pbr
