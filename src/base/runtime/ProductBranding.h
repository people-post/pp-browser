#ifndef PP_BROWSER_PRODUCT_BRANDING_H
#define PP_BROWSER_PRODUCT_BRANDING_H

namespace pbr {

// PP = user-visible product name. pp-browser = internal slug (IDs, paths,
// artifacts, protocols). See docs/ui/PRODUCT_BRANDING.md.
inline constexpr const char* kProductName = "PP";
inline constexpr const char* kProductTagline = "The internet, rendered for you.";
inline constexpr const char* kProductSlug = "pp-browser";
/** Logcat / os_log tag and other dev-facing labels (not the marketing name). */
inline constexpr const char* kProductLogTag = "pp-browser";
/** macOS/iOS .app folder and bundle executable name (matches kProductName). */
inline constexpr const char* kProductBundleName = "PP";
/** Windows AppUserModelID (aligns with macOS bundle id). */
inline constexpr const char* kProductAumid = "dev.pp-browser.app";
inline constexpr const char* kAppIconAsset = "branding/app-icon.png";

} // namespace pbr

#endif // PP_BROWSER_PRODUCT_BRANDING_H
