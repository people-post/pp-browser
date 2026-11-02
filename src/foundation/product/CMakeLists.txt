# The product's identity (CR015 product base, CR023 step 3): its name and slug (ProductBranding.h) and the
# build's version (AppVersion.h). The apps read it and hand capabilities and the base values (CrashIdentity,
# the text layer's {product}); capabilities never include it.
add_library(pp_foundation_product INTERFACE)
target_include_directories(pp_foundation_product INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(pp_foundation_product INTERFACE
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")
