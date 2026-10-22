# Call session orchestration (F004 / F007). Delivery via CallDeliveryPorts;
# conversations binds CallControlInboundPorts — no MeshDeliveryOrchestrator include.
pp_browser_add_feature_library(pp_feature_calls
  SOURCES
    CallMediaBridge.cpp
    CallMediaConnectCoordinator.cpp
    CallMediaPlane.cpp
    CallHopMigrateWorkflow.cpp
    CallSessionWorkflow.cpp
    CallSessionManager.cpp
    CallStack.cpp
    LocalNetworkReaction.cpp
    CallsLoop.cpp
    CallTopologyController.cpp
    CallUiBackend.cpp
    CallPresenter.cpp
    LiveCallMedia.cpp
    CallMediaCoordinator.cpp
    CallControlClient.cpp
    CallInitiationBilling.cpp
    CallReachSignals.cpp
    CallHopRanking.cpp
    CallHopPlanning.cpp
  PUBLIC_LIBS
    pp_foundation_crypto
    pp_domain_media
    pp_domain_messaging
    pp_domain_broadcast
    pp_domain_pricing
    pp_domain_calls
    pp_amp_link
    pp_mesh
    pp_feature_call_media
    pp_domain_people
    pp_foundation_runtime_core
    pp_foundation_runtime)

if(PP_BROWSER_BUILD_TESTS)
  # Harness-based call tests live under feature/conversations/tests and feature/product/tests.
  add_subdirectory(tests)
endif()
