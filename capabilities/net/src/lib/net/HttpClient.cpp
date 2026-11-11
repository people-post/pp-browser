#include "lib/net/HttpClient.h"

#include "common/Diag.h"
#include "common/Metrics.h"
#include "foundation/error/AppError.h"
#include "foundation/crypto/Sha256Hex.h"
#include "foundation/tls/ContentSha256Policy.h"
#include "foundation/error/CurlNetworkError.h"
#include "foundation/tls/CurlSsl.h"

#include "common/PbrCompat.h"

#include "lib/net/PublicOnlySocket.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace pbr {

namespace {

constexpr std::string_view kContentSha256Header = "x-amz-content-sha256";

struct ResponseBuffer {
  std::string body;
  std::optional<size_t> max_bytes;
  bool limit_exceeded = false;
  CURL* curl = nullptr;
  /** Redirects followed when `body` started: a body from before a redirect is not the answer. */
  long body_redirects = 0;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, ResponseBuffer* out) {
  if (size != 0 && nmemb > std::numeric_limits<size_t>::max() / size) {
    out->limit_exceeded = true;
    return 0;
  }
  const size_t total = size * nmemb;
  if (out->curl != nullptr) {
    long redirects = 0;
    curl_easy_getinfo(out->curl, CURLINFO_REDIRECT_COUNT, &redirects);
    if (redirects != out->body_redirects) {
      out->body.clear(); // a new response after a followed redirect: only its body is kept
      out->body_redirects = redirects;
    }
  }
  if (out->max_bytes && (total > *out->max_bytes || out->body.size() > *out->max_bytes - total)) {
    out->limit_exceeded = true;
    return 0;
  }
  out->body.append(static_cast<const char*>(contents), total);
  return total;
}

std::mutex g_ui_probe_mutex;
std::shared_ptr<const std::function<bool()>> g_ui_probe; // guarded by g_ui_probe_mutex

std::function<bool()> CurrentUiProbe() {
  std::lock_guard lock(g_ui_probe_mutex);
  return g_ui_probe ? *g_ui_probe : std::function<bool()>{};
}

/**
 * The routes a `net.ui_block` line may name (UiBlockRoute with `/` as `.`): the mounts of our own backend and the
 * first segments of its APIs. Any other route is written `other`, so a path from elsewhere (a peer's URL) never
 * reaches the line.
 */
constexpr ShareWord kUiBlockRoutes[] = {
    "api.relay",        "api.llm",          "api.support",    "mcp",         "v1.users", "v1.devices", "v1.register",
    "v1.profile",       "v1.search",        "v1.mesh",        "v1.blobs",    "v1.files", "v1.lockers", "v1.inbox",
    "v1.client-compat", "v1.crash-reports", "v1.diagnostics", "guest.start",
};

/** Every public entry point holds one for the whole call: the single place a UI-thread request is recorded. */
class UiBlockWatch {
public:
  explicit UiBlockWatch(const std::string& url) : url_(url) {
    const std::function<bool()> probe = CurrentUiProbe();
    on_ui_ = probe && probe();
    if (on_ui_) {
      start_ = std::chrono::steady_clock::now();
    }
  }
  ~UiBlockWatch() {
    if (!on_ui_) {
      return;
    }
    try {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
      std::string word = HttpClient::UiBlockRoute(url_);
      if (!word.empty() && word.front() == '/') {
        word.erase(0, 1);
      }
      std::replace(word.begin(), word.end(), '/', '.');
      DiagLine("net.ui_block").Pick("route", word, kUiBlockRoutes).Add("ms", ms).Emit();
      MetricsLine("net.ui_block").Add("ms", ms).Emit();
    } catch (...) {
      // A diagnostic line never fails the request it describes.
    }
  }
  UiBlockWatch(const UiBlockWatch&) = delete;
  UiBlockWatch& operator=(const UiBlockWatch&) = delete;

private:
  const std::string& url_;
  bool on_ui_ = false;
  std::chrono::steady_clock::time_point start_;
};

std::atomic<bool> g_first_hop_local_for_test{false};
std::atomic<bool> g_plain_http_redirects_for_test{false};

/**
 * https only, to public addresses only, no proxy: a proxy from the environment would connect to the proxy's
 * address, not the target's, so OpenPublicOnlySocket would be checking the wrong host and a proxy could tunnel
 * to an internal address anyway. Redirects (when followed) get the same checks: the protocol list for them and
 * the socket check on every connection.
 */
void ApplyPublicOnly(CURL* curl, PublicOnlySocketTestExemption& exemption) {
  const bool first_hop_local = g_first_hop_local_for_test.load();
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, first_hop_local ? "http,https" : "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, OpenPublicOnlySocket);
  if (first_hop_local) {
    exemption.opens_left = 1;
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &exemption);
  }
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
}

constexpr long kMaxRedirects = 2;

/** At most two hops, and only to https (whatever `restrict_to_public_https` says; a test may allow http). */
void ApplyFollowRedirects(CURL* curl, const bool restrict_to_public_https) {
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
  const bool plain_http = g_plain_http_redirects_for_test.load() && !restrict_to_public_https;
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, plain_http ? "http,https" : "https");
}

/**
 * A transfer that failed after a redirect was followed: the first server answered (with a followed redirect, the
 * relay spent a download on it), so this is not a "could not reach" failure whatever curl says about the next hop.
 */
Error TransferFailure(const CURLcode code, const long redirect_count) {
  if (redirect_count > 0) {
    return AppError::Network(Err::Network::HttpError,
                             std::string("HTTP request failed after a redirect: ") + curl_easy_strerror(code));
  }
  return AppError::Network(NetworkCodeFromCurl(code), std::string("HTTP request failed: ") + curl_easy_strerror(code));
}

Roe<HttpResponse> Perform(const std::string& url, const char* method, const std::string& body,
                          const std::map<std::string, std::string>& headers, std::optional<size_t> max_response_bytes,
                          HttpTimeout timeout, bool restrict_to_public_https, bool follow_redirects = false,
                          bool add_content_sha256 = false) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return AppError::Internal("Failed to init curl");
  }

  ApplyCurlSslDefaults(curl);

  PublicOnlySocketTestExemption exemption;
  if (restrict_to_public_https) {
    ApplyPublicOnly(curl, exemption);
  }
  if (follow_redirects) {
    ApplyFollowRedirects(curl, restrict_to_public_https);
  }

  ResponseBuffer response_body{.max_bytes = max_response_bytes, .curl = curl};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  const long total_s = timeout.total_s > 0 ? timeout.total_s : 30L;
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_s);
  if (timeout.connect_s > 0) {
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout.connect_s);
  }

  const auto sent_headers =
      add_content_sha256 && SendContentSha256() ? HttpClient::WithContentSha256(method, body, headers) : headers;
  struct curl_slist* header_list = nullptr;
  for (const auto& [key, value] : sent_headers) {
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
  long redirect_count = 0;
  curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirect_count);

  if (header_list) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);

  if (response_body.limit_exceeded) {
    if (!max_response_bytes) {
      return AppError::Network(Err::Network::HttpError, "HTTP response body size overflow");
    }
    return AppError::Network(Err::Network::HttpError, "HTTP response body exceeds configured limit of " +
                                                          std::to_string(*max_response_bytes) + " bytes");
  }

  if (code != CURLE_OK) {
    return TransferFailure(code, redirect_count);
  }

  return HttpResponse{
      .status_code = status_code, .body = std::move(response_body.body), .redirect_count = redirect_count};
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

/** A stream, not a FILE*: `seekg` takes a 64-bit `std::streamoff` on every platform (`fseek` takes `long`). */
struct FileSource {
  std::ifstream file;
  /** Bytes still to send: a range upload stops at its end, not the file's. */
  uint64_t remaining = 0;
  /** The file gave fewer bytes than were still to send (cut short or changed under the upload). */
  bool short_read = false;
};

size_t FileReadCallback(char* buffer, size_t size, size_t nitems, FileSource* source) {
  const uint64_t want = std::min<uint64_t>(static_cast<uint64_t>(size) * nitems, source->remaining);
  source->file.read(buffer, static_cast<std::streamsize>(want));
  const auto got = static_cast<size_t>(source->file.gcount());
  if (got < want) {
    source->short_read = true;
    return CURL_READFUNC_ABORT; // a 0 would tell curl the body ended early and send a short body
  }
  source->remaining -= got;
  return got;
}

/** PUT `source.remaining` bytes from `source.file` (positioned at the start). */
Roe<HttpResponse> PutFromFile(const std::string& url, FileSource& source,
                              const std::map<std::string, std::string>& headers, const HttpProgress& progress) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return AppError::Internal("Failed to init curl");
  }
  ApplyCurlSslDefaults(curl);

  ResponseBuffer response_body{.max_bytes = 64 * 1024};
  ProgressState state{.callback = &progress};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(source.remaining));
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

  if (source.short_read) {
    return AppError::Storage(Err::Storage::Failed, "Upload file is shorter than the bytes it was to send");
  }
  if (state.aborted) {
    return AppError::Network(Err::Network::HttpError, "HTTP upload cancelled");
  }
  if (code != CURLE_OK) {
    return AppError::Network(NetworkCodeFromCurl(code),
                             std::string("HTTP request failed: ") + curl_easy_strerror(code));
  }
  return HttpResponse{.status_code = status_code, .body = std::move(response_body.body)};
}

} // namespace

void HttpClient::SetUiThreadProbe(std::function<bool()> probe) {
  std::lock_guard lock(g_ui_probe_mutex);
  g_ui_probe = probe ? std::make_shared<const std::function<bool()>>(std::move(probe)) : nullptr;
}

HttpClient::ScopedUiThreadProbeForTest::ScopedUiThreadProbeForTest(std::function<bool()> probe)
    : previous_(CurrentUiProbe()) {
  SetUiThreadProbe(std::move(probe));
}

HttpClient::ScopedUiThreadProbeForTest::~ScopedUiThreadProbeForTest() {
  SetUiThreadProbe(std::move(previous_));
}

std::string HttpClient::UiBlockRoute(const std::string& url) {
  std::string_view rest(url);
  if (const size_t scheme = rest.find("://"); scheme != std::string_view::npos) {
    rest.remove_prefix(scheme + 3);
    const size_t path = rest.find_first_of("/?#");
    if (path == std::string_view::npos || rest[path] != '/') {
      return {};
    }
    rest.remove_prefix(path);
  }
  rest = rest.substr(0, rest.find_first_of("?#"));
  std::string route;
  int segments = 0;
  while (segments < 2 && !rest.empty()) {
    while (!rest.empty() && rest.front() == '/') {
      rest.remove_prefix(1);
    }
    const size_t end = rest.find('/');
    const std::string_view segment = rest.substr(0, end);
    if (segment.empty()) {
      break;
    }
    route += '/';
    route += segment;
    ++segments;
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
  }
  return route;
}

void HttpClient::SetStreamingStallLimitForTest(const long min_bytes_per_s, const long window_s) {
  g_min_bytes_per_s.store(min_bytes_per_s > 0 ? min_bytes_per_s : 2048);
  g_speed_window_s.store(window_s > 0 ? window_s : 30);
}

void HttpClient::SetPlainHttpRedirectsForTest(const bool allow) {
  g_plain_http_redirects_for_test.store(allow);
}

void HttpClient::SetFirstHopLocalForTest(const bool allow) {
  g_first_hop_local_for_test.store(allow);
}

Roe<HttpResponse> HttpClient::PutFile(const std::string& url, const std::string& path,
                                      const std::map<std::string, std::string>& headers, const HttpProgress& progress) {
  const UiBlockWatch watch(url);
  std::error_code size_ec;
  const auto file_size = std::filesystem::file_size(path, size_ec);
  if (size_ec) {
    return AppError::Internal("Upload file is not readable");
  }
  FileSource source{.file = std::ifstream(path, std::ios::binary), .remaining = static_cast<uint64_t>(file_size)};
  if (!source.file) {
    return AppError::Internal("Upload file is not readable");
  }
  return PutFromFile(url, source, headers, progress);
}

Roe<HttpResponse> HttpClient::PutFileRange(const std::string& url, const std::string& path, const uint64_t offset,
                                           const uint64_t length, const std::string& content_type,
                                           const HttpProgress& progress) {
  const UiBlockWatch watch(url);
  std::error_code size_ec;
  const auto file_size = std::filesystem::file_size(path, size_ec);
  if (size_ec || offset > static_cast<uint64_t>(file_size) || length > static_cast<uint64_t>(file_size) - offset) {
    return AppError::Internal("Upload file does not hold the part");
  }
  FileSource source{.file = std::ifstream(path, std::ios::binary), .remaining = length};
  if (!source.file || !source.file.seekg(static_cast<std::streamoff>(offset))) {
    return AppError::Internal("Upload file is not readable");
  }
  return PutFromFile(url, source, {{"Content-Type", content_type}}, progress);
}

Roe<HttpFileDownload> HttpClient::GetToFile(const std::string& url, const std::string& dest_path,
                                            const uint64_t max_bytes, const HttpProgress& progress,
                                            const bool restrict_to_public_https,
                                            const std::map<std::string, std::string>& headers,
                                            const bool follow_redirects) {
  const UiBlockWatch watch(url);
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
  PublicOnlySocketTestExemption exemption;
  if (restrict_to_public_https) {
    ApplyPublicOnly(curl, exemption);
  }
  if (follow_redirects) {
    ApplyFollowRedirects(curl, restrict_to_public_https);
  }
  ProgressState state{.callback = &progress};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  // A server that announces more than the cap is refused before a byte is read. Not when following redirects:
  // curl would hold a redirect's own announced length against it too; the sink's cap still stops an overrun.
  if (!follow_redirects) {
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(max_bytes));
  }
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
  long redirect_count = 0;
  curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirect_count);
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
    return fail(AppError::Network(
        Err::Network::HttpError, "HTTP download exceeds the declared size of " + std::to_string(max_bytes) + " bytes"));
  }
  if (state.aborted) {
    return fail(AppError::Network(Err::Network::HttpError, "HTTP download cancelled"));
  }
  if (sink.write_failed) {
    return fail(AppError::Storage(Err::Storage::Failed, "Download file could not be written"));
  }
  if (code != CURLE_OK) {
    return fail(TransferFailure(code, redirect_count));
  }
  if (status_code < 200 || status_code >= 300) {
    std::filesystem::remove(dest_path, ec);
    return HttpFileDownload{.status_code = status_code, .bytes = 0, .redirect_count = redirect_count};
  }
  return HttpFileDownload{.status_code = status_code, .bytes = sink.written, .redirect_count = redirect_count};
}

Roe<HttpResponse> HttpClient::Get(const std::string& url, const std::map<std::string, std::string>& headers,
                                  std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                  bool restrict_to_public_https, const bool follow_redirects) {
  const UiBlockWatch watch(url);
  return Perform(url, "GET", {}, headers, max_response_bytes, timeout, restrict_to_public_https, follow_redirects);
}

Roe<HttpResponse> HttpClient::Post(const std::string& url, const std::string& body,
                                   const std::map<std::string, std::string>& headers,
                                   std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                   bool restrict_to_public_https, const bool add_content_sha256) {
  const UiBlockWatch watch(url);
  return Perform(url, "POST", body, headers, max_response_bytes, timeout, restrict_to_public_https, false,
                 add_content_sha256);
}

Roe<HttpResponse> HttpClient::Put(const std::string& url, const std::string& body,
                                  const std::map<std::string, std::string>& headers,
                                  std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                  bool restrict_to_public_https, const bool add_content_sha256) {
  const UiBlockWatch watch(url);
  return Perform(url, "PUT", body, headers, max_response_bytes, timeout, restrict_to_public_https, false,
                 add_content_sha256);
}

std::map<std::string, std::string> HttpClient::WithContentSha256(const std::string& method, const std::string& body,
                                                                 const std::map<std::string, std::string>& headers) {
  auto out = headers;
  if (method != "POST" && method != "PUT") {
    return out;
  }
  const bool present = std::any_of(out.begin(), out.end(), [](const auto& kv) {
    return kv.first.size() == kContentSha256Header.size() &&
           std::equal(kv.first.begin(), kv.first.end(), kContentSha256Header.begin(),
                      [](const char a, const char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
  });
  if (!present) {
    out.emplace(std::string(kContentSha256Header), Sha256Hex(body));
  }
  return out;
}

} // namespace pbr
