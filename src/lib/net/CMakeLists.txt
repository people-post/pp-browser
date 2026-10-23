pp_browser_add_lib_library(pp_lib_net
  SOURCES
    BlobClient.cpp
    BlobQuota.cpp
    BriefGuestLlmClient.cpp
    ClientCompat.cpp
    HttpBlobClient.cpp
    HttpClient.cpp
    RelayApiSignPayload.cpp
    RelayBlobSignPayload.cpp
    RelayInboxCursor.cpp
    RelaySignBytes.cpp
    RegistrationSignPayload.cpp
    OrgBackendClientFactory.cpp
    OrgBackendClientsImpl.cpp
  PUBLIC_LIBS
    pp_foundation_error
    pp_foundation_data
    pp_foundation_crypto
    pp_foundation_platform_core
    CURL::libcurl)

# Post-DNS SSRF guard socket (OS backend next to HttpClient; see docs/architecture/PLATFORM_CODE.md).
if(WIN32)
  target_sources(pp_lib_net PRIVATE PublicOnlySocket_Win32.cpp)
else()
  target_sources(pp_lib_net PRIVATE PublicOnlySocket_Posix.cpp)
endif()

target_compile_definitions(pp_lib_net PUBLIC
  "PP_BROWSER_RELEASE_VERSION=\"${PP_BROWSER_RELEASE_VERSION}\""
  "PP_BROWSER_VERSION=\"${PP_BROWSER_VERSION}\"")

if(PP_BROWSER_BUILD_TESTS)
  # Mock backend clients: test binaries only, never linked into the app or pp-node.
  pp_browser_add_lib_library(pp_lib_net_testing
    SOURCES
      testing/MockOrgBackendClients.cpp
    PUBLIC_LIBS
      pp_lib_net)
  add_subdirectory(tests)
endif()
