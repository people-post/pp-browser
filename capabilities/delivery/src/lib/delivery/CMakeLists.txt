# Secure delivery (projects/capability-repos CR020): pairwise and public E2E sessions (their key derivation, AAD,
# PSK bundles and fingerprints, auto key establishment), envelopes and their
# signing, the record payload codecs, the control lane's envelopes, the relay's message client — what
# messaging and realtime both ride. Below messaging and calls in the capability order.
pp_browser_add_lib_library(pp_lib_delivery
  SOURCES
    AutoKeyEstablishment.cpp
    CanonicalAad.cpp
    PskBundleCodec.cpp
    PskFingerprint.cpp
    SessionKeyDeriver.cpp
    AttachmentContentCipher.cpp
    BackendClientSet.cpp
    ChatPayloadCodec.cpp
    ChatPayloadValidator.cpp
    ControlLaneEnvelope.cpp
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
    IPeerSessionStore.cpp
    TextSpans.cpp
  PUBLIC_LIBS
    pp_delivery_common
    pp_lib_people
    pp_lib_net
    pp_foundation_error
    pp_foundation_crypto
    pp_foundation_identity
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  # Mock relay and peer-history clients and an in-memory thread store: test binaries only, never linked into the app.
  pp_browser_add_lib_library(pp_lib_delivery_testing
    SOURCES
      testing/InMemoryDeliveryStore.cpp
      testing/MockRelayClients.cpp
    PUBLIC_LIBS
      pp_lib_delivery)
  add_subdirectory(tests)
endif()
