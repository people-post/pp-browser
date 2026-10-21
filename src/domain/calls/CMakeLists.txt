# Call vocabulary, control codec, session and media-key stores, and pure path / hop / migration
# logic. Messaging carries call control without knowing it (docs/architecture/CALLS.md).
pp_browser_add_domain_library(pp_domain_calls
  SOURCES
    CallControlCodec.cpp
    CallControlFamily.cpp
    CallDirectPathLogic.cpp
    CallHopAttachLogic.cpp
    CallHopJoinLogic.cpp
    CallHopPlan.cpp
    CallLifecycleTypes.cpp
    CallMediaKeyExchange.cpp
    CallMediaKeyStore.cpp
    CallMediaSeat.cpp
    CallMobility.cpp
    CallPathMobility.cpp
    CallPathPolicy.cpp
    CallSessionLogic.cpp
    CallSessionQueries.cpp
    CallSessionStore.cpp
    CallTypes.cpp
    LiveCall.cpp
    PeerCapsLogic.cpp
    SoftMigrateLogic.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_data
    pp_foundation_crypto
    sodium
    SQLite::SQLite3)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
