#pragma once

namespace pbr {

/**
 * True only in developer builds. A build is a developer build when it was configured with
 * -DPP_BROWSER_DEV_TOOLS=ON, or bakes a PIN in (-DPP_BROWSER_DEV_PIN, the iOS install script's
 * GCC_PREPROCESSOR_DEFINITIONS): the same builds that skip the unlock prompt. Android: set the
 * PP_BROWSER_DEV_TOOLS=1 environment variable for gradle. Release / TestFlight / store builds set
 * neither, so they never show developer-only pages.
 */
#if defined(PP_BROWSER_DEV_TOOLS) || defined(PP_BROWSER_DEV_PIN)
inline constexpr bool kDevToolsBuild = true;
#else
inline constexpr bool kDevToolsBuild = false;
#endif

} // namespace pbr
