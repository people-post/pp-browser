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
std::string BriefRelayBaseUrl();
std::string BriefMcpUrl();

/** pp-browser vs pp-browser-sandbox (config/data/cache roots). */
const char* ProductDirBasename();

/** When sandbox is on, rewrite production Brief URLs to the sandbox origin. */
std::string RewriteBriefOriginUrl(std::string url);

/** Parse --sandbox from argv; falls back to PP_BROWSER_SANDBOX when unset. */
bool ResolveSandboxModeFromLaunch(int argc, char** argv);

} // namespace pbr
