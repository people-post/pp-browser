#pragma once

#include "common/PlatformLimits.h"
#include "common/net/HttpTransport.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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

/** Bytes moved so far and the total (0: not known); return false to abort the transfer. */
using HttpProgress = std::function<bool(uint64_t done, uint64_t total)>;

/** What a file download ended with: the HTTP status and the bytes written to the file (0 unless 2xx). */
struct HttpFileDownload {
  long status_code = 0;
  uint64_t bytes = 0;
  /** Redirects followed (only with `follow_redirects`). */
  long redirect_count = 0;
};

class HttpClient {
public:
  /**
   * The app's "this is the UI thread" test (AppRuntime::CurrentlyOnUI), set once at startup. Empty (the default:
   * node tools, tests) records nothing. A request made where it answers true held the UI thread for the whole round
   * trip: when it ends (answered or failed) a Diag `net.ui_block` line (`route`: UiBlockRoute as a listed word,
   * `ms`) and a `net.ui_block` metric (`ms`) are written. The request itself is not changed.
   */
  static void SetUiThreadProbe(std::function<bool()> probe);
  /** Tests: the probe for this scope; the previous one (normally none) comes back after it. */
  class ScopedUiThreadProbeForTest {
  public:
    explicit ScopedUiThreadProbeForTest(std::function<bool()> probe);
    ~ScopedUiThreadProbeForTest();
    ScopedUiThreadProbeForTest(const ScopedUiThreadProbeForTest&) = delete;
    ScopedUiThreadProbeForTest& operator=(const ScopedUiThreadProbeForTest&) = delete;

  private:
    std::function<bool()> previous_;
  };
  /**
   * The first two segments of the URL's path, nothing else: no scheme, host, port, query, fragment or later segment
   * ("https://h/v1/users/by-account/x?q=1" -> "/v1/users"; "" when the URL has no path).
   */
  static std::string UiBlockRoute(const std::string& url);

  /** Tests only: the speed floor of PutFile / GetToFile (default 2048 B/s over 30 s); 0, 0 restores it. */
  static void SetStreamingStallLimitForTest(long min_bytes_per_s, long window_s);
  /**
   * Tests only: with `restrict_to_public_https`, the first connection may be a plain-http loopback test server;
   * every later hop (a redirect) is held to https + public addresses as always. Off by default and in the app.
   */
  static void SetFirstHopLocalForTest(bool allow);
  /**
   * Tests only: a followed redirect may go to plain http (a loopback test server) when the request is not
   * `restrict_to_public_https`. Off by default and in the app: redirects go to https only.
   */
  static void SetPlainHttpRedirectsForTest(bool allow);
  /**
   * PUT the file's bytes read straight from disk (never the whole file in memory). No total time limit, only a
   * speed floor (under 2 KB/s for 30 s running fails; see SetStreamingStallLimitForTest), so a large upload on a slow link can finish. The response body
   * is read up to a small cap. `progress` is called as bytes go out.
   */
  static Roe<HttpResponse> PutFile(const std::string& url, const std::string& path,
                                   const std::map<std::string, std::string>& headers = {},
                                   const HttpProgress& progress = {});
  /**
   * PUT `length` bytes of the file starting at `offset` (one part of a multipart upload), read from disk as they
   * go out, with `Content-Type: content_type` and no Content-Range. Speed floor, response cap and `progress`
   * (counting within the range) as for PutFile. The file must hold the whole range. The answer's status is
   * returned as for PutFile; the caller turns a non-2xx into an error.
   */
  static Roe<HttpResponse> PutFileRange(const std::string& url, const std::string& path, uint64_t offset,
                                        uint64_t length, const std::string& content_type,
                                        const HttpProgress& progress = {});
  /**
   * GET into `dest_path`, written as it arrives. Hard cap: if more than `max_bytes` arrive the transfer is aborted
   * and the file removed, whatever the server said it would send. A non-2xx answer writes nothing (the file is
   * removed) and returns the status. Any failure leaves no file behind. Stall limit as for PutFile.
   * `follow_redirects` as for Get.
   */
  static Roe<HttpFileDownload> GetToFile(const std::string& url, const std::string& dest_path, uint64_t max_bytes,
                                         const HttpProgress& progress = {}, bool restrict_to_public_https = false,
                                         const std::map<std::string, std::string>& headers = {},
                                         bool follow_redirects = false);

  /**
   * Pass std::nullopt only when a caller deliberately requires an unbounded response.
   *
   * `restrict_to_public_https`: for URLs sourced from a remote peer (attachment/profile-icon
   * fetch) rather than our own backend config — forces https (initial request and redirects)
   * and rejects any resolved address that is not a public-routable host (loopback/private/
   * link-local/CGNAT/multicast), checked after DNS resolution to also cover DNS rebinding.
   *
   * `follow_redirects` (off by default: a 3xx is returned as the answer): follow up to 2 redirects, to https only.
   * Every hop is held to the same rules (public addresses when restricted), only the final answer's body is kept
   * and the size cap counts it. A failure after a redirect was followed is a Network HttpError (an answer came).
   * The redirect target is never logged or put in an error (a presigned URL is a credential).
   */
  static Roe<HttpResponse> Get(const std::string& url, const std::map<std::string, std::string>& headers = {},
                               std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                               HttpTimeout timeout = {}, bool restrict_to_public_https = false,
                               bool follow_redirects = false);
  /**
   * `headers` plus, for POST and PUT, `x-amz-content-sha256` (hex SHA-256 of `body`, also when empty) unless the
   * caller already set it in any letter case. CloudFront origin access control rejects a POST/PUT without it.
   */
  static std::map<std::string, std::string> WithContentSha256(const std::string& method, const std::string& body,
                                                              const std::map<std::string, std::string>& headers);
  /**
   * With the process-wide SendContentSha256() switch on (default off), POST/PUT carry `x-amz-content-sha256` (see
   * WithContentSha256); `add_content_sha256` false opts a call out (presigned URLs).
   */
  static Roe<HttpResponse> Post(const std::string& url, const std::string& body,
                                const std::map<std::string, std::string>& headers = {},
                                std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                                HttpTimeout timeout = {}, bool restrict_to_public_https = false,
                                bool add_content_sha256 = true);
  static Roe<HttpResponse> Put(const std::string& url, const std::string& body,
                               const std::map<std::string, std::string>& headers = {},
                               std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                               HttpTimeout timeout = {}, bool restrict_to_public_https = false,
                               bool add_content_sha256 = true);
};

} // namespace pbr
