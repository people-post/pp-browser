# Secure delivery (projects/capability-repos CR020): pairwise and public E2E sessions, envelopes and their
# signing, the record payload codecs, the control lane's envelopes, the relay's message client — what
# messaging and realtime both ride. Below messaging and calls in the capability order.
pp_browser_add_lib_library(pp_lib_delivery
  SOURCES
    BackendClientSet.cpp
    ChatPayloadCodec.cpp
    ChatPayloadValidator.cpp
    ControlLaneEnvelope.cpp
    ControlLaneSeenStore.cpp
    DeliveryNotice.cpp
    DirectChatTargetFromContact.cpp
    E2eIngestClassifier.cpp
    E2ePublicSessionLogic.cpp
    E2eRelayPayloadCodec.cpp
    EnvelopeSigner.cpp
    HttpRelayClient.cpp
    OutboundMessage.cpp
    PairwiseFanoutLogic.cpp
    PeerBriefRoute.cpp
    PeerRequestAuth.cpp
    PskRotateCodec.cpp
    PskSessionCoordinator.cpp
    PublicPskLockCoordinator.cpp
    RelayMessageSignPayload.cpp
    RelayWirePayload.cpp
    SqlitePskSessionStore.cpp
    TextSpans.cpp
  PUBLIC_LIBS
    pp_lib_people
    pp_lib_net
    pp_foundation_error
    pp_foundation_crypto
    pp_foundation_identity
    sodium
    SQLite::SQLite3)

if(PP_BROWSER_BUILD_TESTS)
  # Mock relay and peer-history clients: test binaries only, never linked into the app.
  pp_browser_add_lib_library(pp_lib_delivery_testing
    SOURCES
      testing/MockRelayClients.cpp
    PUBLIC_LIBS
      pp_lib_delivery)
  add_subdirectory(tests)
endif()
