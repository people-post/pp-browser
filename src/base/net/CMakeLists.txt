pp_browser_add_base_library(pp_base_net
  SOURCES
    AttachmentFetchUtil.cpp
    AttachmentClientUtil.cpp
    BlobClient.cpp
    BlobQuotaUtil.cpp
    BriefGuestLlmClient.cpp
    ClientCompat.cpp
    CurlSsl.cpp
    HttpBlobClient.cpp
    HttpClient.cpp
    ProfileIconClientUtil.cpp
    ProfileIconFetchUtil.cpp
    RelayApiSignPayload.cpp
    RelayBlobSignPayload.cpp
    RelayInboxCursor.cpp
    RelaySignBytes.cpp
    RegistrationClientUtil.cpp
    RegistrationSignPayload.cpp
    ServiceClientFactory.cpp
    ServiceClientsImpl.cpp
  PUBLIC_LIBS
    pp_base_error
    pp_base_data
    pp_base_crypto
    pp_base_people
    pp_base_messaging
    CURL::libcurl)

target_compile_definitions(pp_base_net PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
