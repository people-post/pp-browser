#ifndef PP_BROWSER_PRODUCT_BRANDING_H
#define PP_BROWSER_PRODUCT_BRANDING_H

namespace pbr {

// User-facing product identity. Internal paths, protocols, and CMake targets
// keep the pp-browser slug for compatibility.
inline constexpr const char* kProductName = "Frame";
inline constexpr const char* kProductTagline = "The internet, rendered for you.";
inline constexpr const char* kProductSlug = "frame";
inline constexpr const char* kAppIconAsset = "branding/app-icon.png";

} // namespace pbr

#endif // PP_BROWSER_PRODUCT_BRANDING_H
