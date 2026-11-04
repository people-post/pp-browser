include(PpBrowserDomain)

# Delivery (CR020): getting an envelope to a peer, for messaging and realtime alike — the mesh transport for
# direct chat (`/pp/chat/…`, AmpDirectChatTransport), account cards over the mesh, and the send path (routes,
# outbox, retries, the control lane). Chat history and the transport bundle are messaging's.
pp_browser_add_domain_library(pp_domain_delivery
  SOURCES
    AmpDirectChatTransport.cpp
    AmpAccountCardTransport.cpp
    AccountCardService.cpp
    PeerRouteBook.cpp
    DeliveryOutbox.cpp
    E2ePublicSessionKeys.cpp
    ControlLane.cpp
  PUBLIC_LIBS
    pp_domain_peers
    pp_lib_delivery
    pp_lib_people
    pp_lib_net
    pp_foundation_crypto
    pp_foundation_runtime_core
    pp_mesh
    pp_amp_link)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
