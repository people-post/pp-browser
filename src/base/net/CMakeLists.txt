pp_browser_add_base_library(pp_base_net
  SOURCES
    BlobClient.cpp
    BlobQuotaUtil.cpp
    BriefGuestLlmClient.cpp
    ClientCompat.cpp
    HttpBlobClient.cpp
    HttpClient.cpp
    RelayApiSignPayload.cpp
    RelayBlobSignPayload.cpp
    RelayInboxCursor.cpp
    RelaySignBytes.cpp
    RegistrationSignPayload.cpp
    ServiceClientFactory.cpp
    ServiceClientsImpl.cpp
  PUBLIC_LIBS
    pp_base_error
    pp_base_data
    pp_base_crypto
    pp_base_platform_core
    CURL::libcurl)

target_compile_definitions(pp_base_net PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
