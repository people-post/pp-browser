# Call session orchestration (F004 / F007). Delivery via CallDeliveryPorts;
# conversations binds CallControlInboundPorts — no MeshDeliveryOrchestrator include.
pp_browser_add_feature_library(pp_feature_calls
  SOURCES
    CallConnectDiag.cpp
    CallDirectPath.cpp
    CallMediaConnectCoordinator.cpp
    CallMediaDoorway.cpp
    CallHopService.cpp
    CallMediaPlane.cpp
    CallSessionWorkflow.cpp
    CallDevice.cpp
    CallEngine.cpp
    CallSessionManager.cpp
    CallReads.cpp
    CallStack.cpp
    LocalNetworkReaction.cpp
    CallsLoop.cpp
    CallHopMediaChannel.cpp
    CallHopPath.cpp
    CallUiBackend.cpp
    LiveCallMedia.cpp
    CallMedia.cpp
    CallControlClient.cpp
    CallInitiationBilling.cpp
    CallReachSignals.cpp
    CallHopRanking.cpp
    CallHopPlanning.cpp
  PUBLIC_LIBS
    pp_foundation_crypto
    pp_lib_media
    pp_lib_delivery
    pp_lib_pricing
    pp_lib_calls
    pp_amp_link
    pp_mesh
    pp_domain_call_media
    pp_lib_people
    pp_foundation_runtime_core
    pp_foundation_runtime)

if(PP_BROWSER_BUILD_TESTS)
  # Harness-based call tests live under feature/conversations/tests and feature/product/tests.
  add_subdirectory(tests)
endif()
