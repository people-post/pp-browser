#include "base/net/HttpClient.h"

#include <curl/curl.h>

namespace pbr {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
  const size_t total = size * nmemb;
  out->append(static_cast<char*>(contents), total);
  return total;
}

Roe<HttpResponse> Perform(const std::string& url, const char* method, const std::string& body,
                          const std::map<std::string, std::string>& headers) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return Error("Failed to init curl");
  }

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  struct curl_slist* header_list = nullptr;
  for (const auto& [key, value] : headers) {
    header_list = curl_slist_append(header_list, (key + ": " + value).c_str());
  }
  if (header_list) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  }

  const CURLcode code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

  if (header_list) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);

  if (code != CURLE_OK) {
    return Error(std::string("HTTP request failed: ") + curl_easy_strerror(code));
  }

  return HttpResponse{.status_code = status_code, .body = std::move(response_body)};
}

} // namespace

Roe<HttpResponse> HttpClient::Get(const std::string& url, const std::map<std::string, std::string>& headers) {
  return Perform(url, "GET", {}, headers);
}

Roe<HttpResponse> HttpClient::Post(const std::string& url, const std::string& body,
                                   const std::map<std::string, std::string>& headers) {
  return Perform(url, "POST", body, headers);
}

} // namespace pbr
