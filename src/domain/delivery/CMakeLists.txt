include(PpBrowserDomain)

# Delivery: getting a chat envelope to a peer and the peer's history back, for any feature — the
# mesh transports for direct chat (`/pp/chat/…`, AmpDirectChatTransport) and chat history
# (AmpChatHistoryTransport). The send path (routes, outbox, retries, the control lane) follows.
pp_browser_add_domain_library(pp_domain_delivery
  SOURCES
    AmpDirectChatTransport.cpp
    AmpAccountCardTransport.cpp
    AccountCardService.cpp
    AmpChatHistoryTransport.cpp
    PeerRouteBook.cpp
    PeerTransports.cpp
    DeliveryOutbox.cpp
    E2ePublicSessionKeys.cpp
    ControlLane.cpp
  PUBLIC_LIBS
    pp_domain_peers
    pp_domain_attachments
    pp_lib_messaging
    pp_lib_people
    pp_lib_net
    pp_foundation_crypto
    pp_foundation_runtime_core
    pp_mesh
    pp_amp_link)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
