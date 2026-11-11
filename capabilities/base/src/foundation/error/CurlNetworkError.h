#pragma once

#include "foundation/error/AppError.h"

namespace pbr {

/**
 * Map a libcurl result code (CURLcode, passed as int so callers need no curl header here) to the
 * network error code that names its cause: DNS, connect, timeout, offline, TLS (certificate / verification failures only). Anything else is Unreachable.
 */
Err::Network NetworkCodeFromCurl(int curl_code);

} // namespace pbr
