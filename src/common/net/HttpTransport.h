#pragma once

#include "common/Error.h"

#include <functional>
#include <map>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct HttpResponse {
  long status_code = 0;
  std::string body;
};

/**
 * Injectable HTTP POST used by lib peers that must not include net/HttpClient.
 * `timeout_s`: the whole request's bound in seconds; 0 = the transport's default.
 */
using HttpPostFn = std::function<Roe<HttpResponse>(const std::string& url, const std::string& body,
                                                   const std::map<std::string, std::string>& headers,
                                                   long timeout_s)>;

} // namespace pbr
