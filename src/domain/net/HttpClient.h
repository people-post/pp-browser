#pragma once

#include "common/PlatformLimits.h"
#include "common/net/HttpTransport.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace pbr {

/** curl timeouts for a single request (coded policy — not env knobs). */
struct HttpTimeout {
  long total_s = 30;
  /** 0 = libcurl default connect timeout. */
  long connect_s = 0;
};

class HttpClient {
public:
  /** Pass std::nullopt only when a caller deliberately requires an unbounded response. */
  static Roe<HttpResponse> Get(const std::string& url, const std::map<std::string, std::string>& headers = {},
                               std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                               HttpTimeout timeout = {});
  static Roe<HttpResponse> Post(const std::string& url, const std::string& body,
                                const std::map<std::string, std::string>& headers = {},
                                std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                                HttpTimeout timeout = {});
  static Roe<HttpResponse> Put(const std::string& url, const std::string& body,
                               const std::map<std::string, std::string>& headers = {},
                               std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                               HttpTimeout timeout = {});
};

} // namespace pbr
