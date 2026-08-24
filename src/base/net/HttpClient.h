#pragma once

#include "common/Error.h"

#include <map>
#include <string>

namespace pbr {

struct HttpResponse {
  long status_code = 0;
  std::string body;
};

class HttpClient {
public:
  static Roe<HttpResponse> Get(const std::string& url, const std::map<std::string, std::string>& headers = {});
  static Roe<HttpResponse> Post(const std::string& url, const std::string& body,
                                const std::map<std::string, std::string>& headers = {});
  static Roe<HttpResponse> Put(const std::string& url, const std::string& body,
                               const std::map<std::string, std::string>& headers = {});
};

} // namespace pbr
