#ifndef PP_BROWSER_APP_VERSION_H
#define PP_BROWSER_APP_VERSION_H

namespace pbr {

#ifndef PP_BROWSER_RELEASE_VERSION
#define PP_BROWSER_RELEASE_VERSION "0.1.0"
#endif

#ifndef PP_BROWSER_VERSION
#define PP_BROWSER_VERSION "0.1.0"
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
 * Monotonic wire/capability generation advertised to peers and compared against
 * `min_protocol_gen` / `min_peer_protocol_gen`. Bump only when intentionally cutting old peers.
 */
inline constexpr int kProtocolGen = 1;

/** Minimum peer protocol_gen this build accepts for full peer features. */
inline constexpr int kMinPeerProtocolGen = 1;

inline constexpr const char* kDefaultUpgradeUrl =
    "https://github.com/people-post/pp-browser/releases";

} // namespace pbr

#endif // PP_BROWSER_APP_VERSION_H
