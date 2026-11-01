#pragma once

#include <string>

namespace pbr {

inline constexpr const char* kProductDirName = "pp-browser";

/**
 * Backend origin (scheme://host, no trailing slash). PP_BROWSER_ORIGIN wins over the compile-time
 * default (CMake PP_BROWSER_DEFAULT_ORIGIN); both unset yields "". Read on every call.
 * All URLs derived from it are empty when the origin is empty, which disables the backend features.
 */
std::string BriefOrigin();
/** The compile-time default origin ("" unless configured). */
const char* CompiledDefaultOrigin();

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
