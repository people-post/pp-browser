pp_browser_add_domain_library(pp_domain_net
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
    pp_foundation_error
    pp_foundation_data
    pp_foundation_crypto
    pp_foundation_platform_core
    CURL::libcurl)

target_compile_definitions(pp_domain_net PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
