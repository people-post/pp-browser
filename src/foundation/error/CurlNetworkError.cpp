#include "foundation/error/CurlNetworkError.h"

#include <curl/curl.h>

namespace pbr {

Err::Network NetworkCodeFromCurl(int curl_code) {
  switch (static_cast<CURLcode>(curl_code)) {
  case CURLE_COULDNT_RESOLVE_HOST:
    return Err::Network::DnsFailed;
  case CURLE_COULDNT_CONNECT:
    return Err::Network::ConnectFailed;
  case CURLE_OPERATION_TIMEDOUT:
    return Err::Network::Timeout;
  case CURLE_SSL_CONNECT_ERROR:
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_ISSUER_ERROR:
  case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
  case CURLE_SSL_INVALIDCERTSTATUS:
    return Err::Network::TlsFailed;
  default:
    // Includes local-configuration failures (bad CA file, proxy not resolvable, bad URL) that no
    // network hint would describe correctly.
    return Err::Network::Unreachable;
  }
}

} // namespace pbr
