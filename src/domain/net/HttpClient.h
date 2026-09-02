#pragma once

#include "common/net/HttpTransport.h"

#include <map>
#include <string>

namespace pbr {

class HttpClient {
public:
  static Roe<HttpResponse> Get(const std::string& url, const std::map<std::string, std::string>& headers = {});
  static Roe<HttpResponse> Post(const std::string& url, const std::string& body,
                                const std::map<std::string, std::string>& headers = {});
  static Roe<HttpResponse> Put(const std::string& url, const std::string& body,
                               const std::map<std::string, std::string>& headers = {});
};

} // namespace pbr
