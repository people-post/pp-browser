# Broadcast (media-client-layers L001 / B008): sibling of calls — viewer today, broadcaster in l5.
# Shares only neutral layers (domain/mesh reach + media_relay attach, domain/media pipelines and
# device leases); never links pp_feature_calls.
pp_browser_add_feature_library(pp_feature_broadcast
  SOURCES
    BroadcastHub.cpp
    BroadcastViewerWorkflow.cpp
  PUBLIC_LIBS
    pp_domain_media
    pp_domain_messaging
    pp_domain_mesh
    pp_foundation_runtime_core)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
