# The platform's TLS trust for curl (CR015: a mechanism capabilities need, in the base): iOS verifies the peer
# chain with Security.framework (SecTrust, BoringSSL has no store), Android points curl at the system CA directory,
# desktop keeps curl's own. Net's HTTP client and the app's AI clients use it.
set(_pp_tls_sources CurlSsl.cpp ContentSha256Policy.cpp)
if(PP_BROWSER_IS_ANDROID)
  list(APPEND _pp_tls_sources OsTlsPlatformCurl_Android.cpp OsTlsCaPath_Android.cpp)
elseif(PP_BROWSER_IS_IOS)
  list(APPEND _pp_tls_sources OsTlsPlatformCurl_Ios.cpp OsTlsCaPath_Default.cpp)
else()
  list(APPEND _pp_tls_sources OsTlsPlatformCurl_Default.cpp OsTlsCaPath_Default.cpp)
endif()

pp_browser_add_foundation_library(pp_foundation_tls
  SOURCES ${_pp_tls_sources}
  PUBLIC_LIBS CURL::libcurl)

# iOS: the SecTrust verify callback compiles against BoringSSL's headers (B8).
if(PP_BROWSER_IS_IOS)
  target_link_libraries(pp_foundation_tls PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    "-framework Security"
    "-framework CoreFoundation")
endif()
