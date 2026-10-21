# Live broadcast: join tickets, viewer ladders, relay media, the broadcast RPC codec, and the
# signed peer-announce feed that advertises programs (projects/peer-scoped-broadcast/).
pp_browser_add_domain_library(pp_domain_broadcast
  SOURCES
    AnnounceNotificationInbox.cpp
    AnnounceOverlayReply.cpp
    BroadcastJoinTicket.cpp
    BroadcastLadderLogic.cpp
    BroadcastMedia.cpp
    BroadcastRpcCodec.cpp
    BroadcastViewerLadder.cpp
    PeerAnnounceCodec.cpp
    PeerAnnounceFeed.cpp
    PeerAnnounceKeyResolve.cpp
    PeerAnnouncePublisher.cpp
    PeerAnnounceRpcCodec.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_crypto
    sodium)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
