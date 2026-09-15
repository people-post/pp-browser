#pragma once

#include "common/PlatformLimits.h"
#include "common/net/HttpTransport.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace pbr {

class HttpClient {
public:
  /** Pass std::nullopt only when a caller deliberately requires an unbounded response. */
  static Roe<HttpResponse> Get(const std::string& url, const std::map<std::string, std::string>& headers = {},
                               std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes);
  static Roe<HttpResponse> Post(const std::string& url, const std::string& body,
                                const std::map<std::string, std::string>& headers = {},
                                std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes);
  static Roe<HttpResponse> Put(const std::string& url, const std::string& body,
                               const std::map<std::string, std::string>& headers = {},
                               std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes);
};

} // namespace pbr
