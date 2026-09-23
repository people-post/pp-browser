#include "domain/net/HttpClient.h"

#include "foundation/error/AppError.h"
#include "foundation/platform/CurlSsl.h"

#include "common/PbrCompat.h"

#include <curl/curl.h>

#include <limits>
#include <optional>

namespace pbr {

namespace {

struct ResponseBuffer {
  std::string body;
  std::optional<size_t> max_bytes;
  bool limit_exceeded = false;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, ResponseBuffer* out) {
  if (size != 0 && nmemb > std::numeric_limits<size_t>::max() / size) {
    out->limit_exceeded = true;
    return 0;
  }
  const size_t total = size * nmemb;
  if (out->max_bytes &&
      (total > *out->max_bytes || out->body.size() > *out->max_bytes - total)) {
    out->limit_exceeded = true;
    return 0;
  }
  out->body.append(static_cast<const char*>(contents), total);
  return total;
}

Roe<HttpResponse> Perform(const std::string& url, const char* method, const std::string& body,
                          const std::map<std::string, std::string>& headers,
                          std::optional<size_t> max_response_bytes, HttpTimeout timeout) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return AppError::Internal("Failed to init curl");
  }

  ApplyCurlSslDefaults(curl);

  ResponseBuffer response_body{.max_bytes = max_response_bytes};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  const long total_s = timeout.total_s > 0 ? timeout.total_s : 30L;
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_s);
  if (timeout.connect_s > 0) {
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout.connect_s);
  }

  struct curl_slist* header_list = nullptr;
  for (const auto& [key, value] : headers) {
    header_list = curl_slist_append(header_list, (key + ": " + value).c_str());
  }
  if (header_list) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }

  const CURLcode code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

  if (header_list) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);

  if (response_body.limit_exceeded) {
    if (!max_response_bytes) {
      return AppError::Network(Err::Network::HttpError, "HTTP response body size overflow");
    }
    return AppError::Network(Err::Network::HttpError,
                             "HTTP response body exceeds configured limit of " +
                                 std::to_string(*max_response_bytes) + " bytes");
  }

  if (code != CURLE_OK) {
    return AppError::Network(Err::Network::Unreachable,
                          std::string("HTTP request failed: ") + curl_easy_strerror(code));
  }

  return HttpResponse{.status_code = status_code, .body = std::move(response_body.body)};
}

} // namespace

Roe<HttpResponse> HttpClient::Get(const std::string& url, const std::map<std::string, std::string>& headers,
                                  std::optional<size_t> max_response_bytes, HttpTimeout timeout) {
  return Perform(url, "GET", {}, headers, max_response_bytes, timeout);
}

Roe<HttpResponse> HttpClient::Post(const std::string& url, const std::string& body,
                                   const std::map<std::string, std::string>& headers,
                                   std::optional<size_t> max_response_bytes, HttpTimeout timeout) {
  return Perform(url, "POST", body, headers, max_response_bytes, timeout);
}

Roe<HttpResponse> HttpClient::Put(const std::string& url, const std::string& body,
                                  const std::map<std::string, std::string>& headers,
                                  std::optional<size_t> max_response_bytes, HttpTimeout timeout) {
  return Perform(url, "PUT", body, headers, max_response_bytes, timeout);
}

} // namespace pbr
