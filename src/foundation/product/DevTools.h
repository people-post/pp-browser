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

/**
 * True only in the UI demo app (-DPP_BROWSER_UI_DEMO=ON, iOS simulator / Android; cmake/UiDemo.cmake): a separate
 * developer-only app with fixed fake data and no backend address. Every demo decision lives in an always-compiled
 * function that takes a bool (ResolveBriefOrigin, UploadAllowed, ApplyUiDemoConfig, DemoDataSeeder); this constant only
 * decides what the call sites pass, so the normal build's tests cover the demo behaviour.
 */
#if defined(PP_BROWSER_UI_DEMO)
inline constexpr bool kUiDemoBuild = true;
#else
inline constexpr bool kUiDemoBuild = false;
#endif

/**
 * A relay a person types (My relays) or a QR code names may be a plain `http://` one or at a local address (this
 * device, a private network): developer builds and debug builds only (the lab's plain relays); never on the card
 * (multi-relay MR004). Release builds take neither.
 */
#ifdef NDEBUG
inline constexpr bool kDeveloperRelaysAllowed = kDevToolsBuild;
#else
inline constexpr bool kDeveloperRelaysAllowed = true;
#endif

} // namespace pbr
