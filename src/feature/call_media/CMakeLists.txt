include(PpBrowserFeature)

# Call media: pp-browser's realtime protocol on the mesh's links — the Amp transport, the per-call
# leg coordinator (bundles, failover, frame crypto). A client of the mesh (its io lane, L4 helpers),
# not part of it (docs/architecture/MESH.md § Extraction). Headless: pp-call-probe links it.
pp_browser_add_headless_feature_library(pp_feature_call_media
  SOURCES
    CallMediaAmpTransport.cpp
    CallMediaFrameCrypto.cpp
    CallMediaLegCoordinator.cpp
  PUBLIC_LIBS
    pp_lib_calls
    pp_mesh_shared
    pp_foundation_crypto
    pp_amp_link)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
