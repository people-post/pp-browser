#ifndef PP_BROWSER_PRODUCT_BRANDING_H
#define PP_BROWSER_PRODUCT_BRANDING_H

namespace pbr {

// Frame = user-visible product name. pp-browser = internal slug (IDs, paths,
// artifacts, protocols). See docs/ui/PRODUCT_BRANDING.md.
inline constexpr const char* kProductName = "Frame";
inline constexpr const char* kProductTagline = "The internet, rendered for you.";
inline constexpr const char* kProductSlug = "pp-browser";
/** Windows AppUserModelID (aligns with macOS bundle id). */
inline constexpr const char* kProductAumid = "dev.pp-browser.app";
inline constexpr const char* kAppIconAsset = "branding/app-icon.png";

} // namespace pbr

#endif // PP_BROWSER_PRODUCT_BRANDING_H
