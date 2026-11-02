#ifndef PP_BROWSER_APP_VERSION_H
#define PP_BROWSER_APP_VERSION_H

namespace pbr {

// The build's version, from pp_foundation_product (CMake PP_BROWSER_RELEASE_VERSION / PP_BROWSER_VERSION). No
// fallback: a target that reads the version without linking it would ship a made-up one.
#if !defined(PP_BROWSER_RELEASE_VERSION) || !defined(PP_BROWSER_VERSION)
#error "AppVersion.h needs the build's version: link pp_foundation_product"
#endif

/** Full release string (may include -rc / -beta); used for About and User-Agent. */
inline constexpr const char* AppVersionString() {
  return PP_BROWSER_RELEASE_VERSION;
}

/** Numeric project version without prerelease suffix (CMake `PP_BROWSER_VERSION`). */
inline constexpr const char* AppVersionCoreString() {
  return PP_BROWSER_VERSION;
}

/**
 * Monotonic wire/capability generation for a future in-band messaging hello.
 * Bump only when intentionally cutting old peers. Not advertised via directory.
 */
inline constexpr int kProtocolGen = 1;

/** Minimum peer protocol_gen this build will accept once in-band hello exists. */
inline constexpr int kMinPeerProtocolGen = 1;

inline constexpr const char* kDefaultUpgradeUrl =
    "https://github.com/people-post/pp-browser/releases";

} // namespace pbr

#endif // PP_BROWSER_APP_VERSION_H
