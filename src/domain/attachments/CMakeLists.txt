include(PpBrowserDomain)

# Chat attachments for any feature: the peer-direct blob request and its mesh transport
# (`/pp/blob/1.0.0`, AmpChatBlobTransport), the CDN upload / fetch clients, background fetch +
# decrypt (AttachmentFetchWorkflow), and the content store binding (AttachmentContent → lib/content).
pp_browser_add_domain_library(pp_domain_attachments
  SOURCES
    AmpChatBlobTransport.cpp
    AttachmentClient.cpp
    AttachmentContent.cpp
    AttachmentFetch.cpp
    AttachmentFetchWorkflow.cpp
    ChatBlobRequest.cpp
    LargeTransferLane.cpp
    OutgoingAttachmentStore.cpp
  PUBLIC_LIBS
    pp_lib_messaging
    pp_lib_people
    pp_lib_net
    pp_lib_content
    pp_foundation_crypto
    pp_foundation_platform_core
    pp_foundation_runtime_core
    pp_mesh
    pp_amp_link)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
