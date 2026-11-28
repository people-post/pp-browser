include(PpBrowserBase)

# The product's data layout (capability-repos CR028): where the apps keep their files (AppPaths), the profiles and
# their manifests (one profile model for both apps), the mesh role, and the config file's sections both apps read
# (ConfigSectionsJson: mesh, service endpoints, a custom backend's services). The desktop's own config stays in
# src/foundation/data.
pp_browser_add_foundation_library(pp_product_data
  SOURCES
    AppPaths.cpp
    ConfigSectionsJson.cpp
    MeshRole.cpp
    ProfileManifest.cpp
    ProfileRegistry.cpp
  PUBLIC_LIBS
    pp_product_platform)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
