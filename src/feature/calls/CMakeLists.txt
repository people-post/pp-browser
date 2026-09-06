# Call session orchestration (F004 / F007). Delivery via CallDeliveryPorts;
# conversations binds CallControlInboundPorts — no MeshDeliveryOrchestrator include.
pp_browser_add_feature_library(pp_feature_calls
  SOURCES
    AmpCircuitHopReach.cpp
    AmpMediaRelayClient.cpp
    CallMediaBridge.cpp
    CallLifecycle.cpp
    BroadcastSessionCoordinator.cpp
    CallInboundHandlers.cpp
    CallSessionManager.cpp
    CallStack.cpp
    CallTopologyController.cpp
    CallUiBackend.cpp
  PUBLIC_LIBS
    pp_foundation_crypto
    pp_domain_media
    pp_domain_messaging
    pp_amp_link
    pp_domain_mesh
    pp_domain_people
    pp_foundation_runtime_core
    pp_foundation_runtime)

if(PP_BROWSER_BUILD_TESTS)
  # Call unit tests currently live under feature/conversations/tests (shared harness).
endif()
