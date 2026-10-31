#include "lib/net/HttpClient.h"

#include "foundation/error/AppError.h"
#include "foundation/error/CurlNetworkError.h"
#include "foundation/platform/CurlSsl.h"

#include "common/PbrCompat.h"

#include "lib/net/PublicOnlySocket.h"

#include <curl/curl.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
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
                          std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                          bool restrict_to_public_https) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return AppError::Internal("Failed to init curl");
  }

  ApplyCurlSslDefaults(curl);

  if (restrict_to_public_https) {
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, OpenPublicOnlySocket);
    // A proxy from the environment would connect to the proxy's address, not the target's, so
    // OpenPublicOnlySocket would be checking the wrong host and a proxy could tunnel to an
    // internal address anyway.
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
  }

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
    return AppError::Network(NetworkCodeFromCurl(code),
                             std::string("HTTP request failed: ") + curl_easy_strerror(code));
  }

  return HttpResponse{.status_code = status_code, .body = std::move(response_body.body)};
}

std::atomic<long> g_min_bytes_per_s{2048};
std::atomic<long> g_speed_window_s{30};

/**
 * Streaming transfers have no total time limit, but a link that connects slowly (15 s) or averages under
 * 2 KB/s over 30 s is a failure: a drip must not hold a transfer thread for hours.
 */
void ApplyStallLimit(CURL* curl) {
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, g_min_bytes_per_s.load());
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, g_speed_window_s.load());
}

void ApplyPublicOnly(CURL* curl) {
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, OpenPublicOnlySocket);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
}

struct ProgressState {
  const HttpProgress* callback = nullptr;
  bool aborted = false;
};

int XferInfo(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
  auto* state = static_cast<ProgressState*>(clientp);
  if (state->callback && *state->callback) {
    const bool upload = ultotal > 0;
    const uint64_t done = static_cast<uint64_t>(upload ? ulnow : dlnow);
    const uint64_t total = static_cast<uint64_t>(upload ? ultotal : dltotal);
    if (!(*state->callback)(done, total)) {
      state->aborted = true;
      return 1;
    }
  }
  return 0;
}

struct FileSink {
  std::FILE* file = nullptr;
  CURL* curl = nullptr;
  uint64_t max_bytes = 0;
  uint64_t written = 0;
  bool exceeded = false;
  bool write_failed = false;
};

size_t FileWriteCallback(void* contents, size_t size, size_t nmemb, FileSink* sink) {
  const size_t total = size * nmemb;
  long status = 0;
  curl_easy_getinfo(sink->curl, CURLINFO_RESPONSE_CODE, &status);
  if (status < 200 || status >= 300) {
    return total; // an error page is not the file: discard it
  }
  if (sink->written + total > sink->max_bytes) {
    sink->exceeded = true;
    return 0; // abort: more than the declared size (plus tag) is arriving
  }
  if (total > 0 && std::fwrite(contents, 1, total, sink->file) != total) {
    sink->write_failed = true;
    return 0;
  }
  sink->written += total;
  return total;
}

struct FileSource {
  std::FILE* file = nullptr;
};

size_t FileReadCallback(char* buffer, size_t size, size_t nitems, FileSource* source) {
  return std::fread(buffer, 1, size * nitems, source->file);
}

} // namespace

void HttpClient::SetStreamingStallLimitForTest(const long min_bytes_per_s, const long window_s) {
  g_min_bytes_per_s.store(min_bytes_per_s > 0 ? min_bytes_per_s : 2048);
  g_speed_window_s.store(window_s > 0 ? window_s : 30);
}

Roe<HttpResponse> HttpClient::PutFile(const std::string& url, const std::string& path,
                                      const std::map<std::string, std::string>& headers,
                                      const HttpProgress& progress) {
  std::error_code size_ec;
  const auto file_size = std::filesystem::file_size(path, size_ec);
  if (size_ec) {
    return AppError::Internal("Upload file is not readable");
  }
  FileSource source;
  source.file = std::fopen(path.c_str(), "rb");
  if (!source.file) {
    return AppError::Internal("Upload file is not readable");
  }
  CURL* curl = curl_easy_init();
  if (!curl) {
    std::fclose(source.file);
    return AppError::Internal("Failed to init curl");
  }
  ApplyCurlSslDefaults(curl);

  ResponseBuffer response_body{.max_bytes = 64 * 1024};
  ProgressState state{.callback = &progress};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(file_size));
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, FileReadCallback);
  curl_easy_setopt(curl, CURLOPT_READDATA, &source);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfo);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  ApplyStallLimit(curl);
  struct curl_slist* header_list = nullptr;
  for (const auto& [key, value] : headers) {
    header_list = curl_slist_append(header_list, (key + ": " + value).c_str());
  }
  if (header_list) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  const CURLcode code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  if (header_list) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);
  std::fclose(source.file);

  if (state.aborted) {
    return AppError::Network(Err::Network::HttpError, "HTTP upload cancelled");
  }
  if (code != CURLE_OK) {
    return AppError::Network(NetworkCodeFromCurl(code),
                             std::string("HTTP request failed: ") + curl_easy_strerror(code));
  }
  return HttpResponse{.status_code = status_code, .body = std::move(response_body.body)};
}

Roe<HttpFileDownload> HttpClient::GetToFile(const std::string& url, const std::string& dest_path,
                                            const uint64_t max_bytes, const HttpProgress& progress,
                                            const bool restrict_to_public_https,
                                            const std::map<std::string, std::string>& headers) {
  std::error_code ec;
  std::filesystem::remove(dest_path, ec);
  FileSink sink;
  sink.max_bytes = max_bytes;
  sink.file = std::fopen(dest_path.c_str(), "wb");
  if (!sink.file) {
    return AppError::Internal("Download file cannot be created");
  }
  CURL* curl = curl_easy_init();
  if (!curl) {
    std::fclose(sink.file);
    std::filesystem::remove(dest_path, ec);
    return AppError::Internal("Failed to init curl");
  }
  sink.curl = curl;
  ApplyCurlSslDefaults(curl);
  if (restrict_to_public_https) {
    ApplyPublicOnly(curl);
  }
  ProgressState state{.callback = &progress};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  // A server that announces more than the cap is refused before a byte is read.
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(max_bytes));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FileWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfo);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  ApplyStallLimit(curl);
  struct curl_slist* header_list = nullptr;
  for (const auto& [key, value] : headers) {
    header_list = curl_slist_append(header_list, (key + ": " + value).c_str());
  }
  if (header_list) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  const CURLcode code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  if (header_list) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);
  std::fclose(sink.file);

  const auto fail = [&](Error error) -> Roe<HttpFileDownload> {
    std::filesystem::remove(dest_path, ec); // an overrun or broken transfer leaves nothing behind
    return error;
  };
  if (sink.exceeded || code == CURLE_FILESIZE_EXCEEDED) {
    return fail(AppError::Network(Err::Network::HttpError,
                                  "HTTP download exceeds the declared size of " + std::to_string(max_bytes) + " bytes"));
  }
  if (state.aborted) {
    return fail(AppError::Network(Err::Network::HttpError, "HTTP download cancelled"));
  }
  if (sink.write_failed) {
    return fail(AppError::Storage(Err::Storage::Failed, "Download file could not be written"));
  }
  if (code != CURLE_OK) {
    return fail(AppError::Network(NetworkCodeFromCurl(code),
                                  std::string("HTTP request failed: ") + curl_easy_strerror(code)));
  }
  if (status_code < 200 || status_code >= 300) {
    std::filesystem::remove(dest_path, ec);
    return HttpFileDownload{.status_code = status_code, .bytes = 0};
  }
  return HttpFileDownload{.status_code = status_code, .bytes = sink.written};
}

Roe<HttpResponse> HttpClient::Get(const std::string& url, const std::map<std::string, std::string>& headers,
                                  std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                  bool restrict_to_public_https) {
  return Perform(url, "GET", {}, headers, max_response_bytes, timeout, restrict_to_public_https);
}

Roe<HttpResponse> HttpClient::Post(const std::string& url, const std::string& body,
                                   const std::map<std::string, std::string>& headers,
                                   std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                   bool restrict_to_public_https) {
  return Perform(url, "POST", body, headers, max_response_bytes, timeout, restrict_to_public_https);
}

Roe<HttpResponse> HttpClient::Put(const std::string& url, const std::string& body,
                                  const std::map<std::string, std::string>& headers,
                                  std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                  bool restrict_to_public_https) {
  return Perform(url, "PUT", body, headers, max_response_bytes, timeout, restrict_to_public_https);
}

} // namespace pbr
