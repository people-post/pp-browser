# Call vocabulary, control codec, session and media-key stores, and pure path / hop / migration
# logic. Messaging carries call control without knowing it (docs/architecture/CALLS.md).
pp_browser_add_lib_library(pp_lib_calls
  SOURCES
    MediaKeyWrap.cpp
    CallControlCodec.cpp
    CallControlFamily.cpp
    CallMediaErrorText.cpp
    CallDirectPathLogic.cpp
    CallHopAttachLogic.cpp
    CallHopJoinLogic.cpp
    CallHopPlan.cpp
    CallLifecycleTypes.cpp
    CallMediaBundleLogic.cpp
    CallMediaControlCodec.cpp
    CallMediaKeyExchange.cpp
    ICallMediaKeyStore.cpp
    CallMediaSeat.cpp
    CallMediaSessionLogic.cpp
    CallMobility.cpp
    CallPathMobility.cpp
    CallPathPolicy.cpp
    CallScreen.cpp
    CallSessionLogic.cpp
    CallSessionQueries.cpp
    CallSummaryLogic.cpp
    CallTypes.cpp
    LiveCall.cpp
    PeerCapsLogic.cpp
    SoftMigrateLogic.cpp
  PUBLIC_LIBS
    pp_calls_common
    pp_delivery_common
    pp_foundation_error
    pp_foundation_crypto
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  # Calls' stores in memory (sessions, media keys): test binaries only, never linked into the app.
  pp_browser_add_lib_library(pp_lib_calls_testing
    SOURCES
      testing/InMemoryCallStores.cpp
    PUBLIC_LIBS
      pp_lib_calls)
  add_subdirectory(tests)
endif()
