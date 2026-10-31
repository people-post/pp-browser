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
};

class HttpClient {
public:
  /**
   * PUT the file's bytes read straight from disk (never the whole file in memory). No total time limit, only a
   * stall limit (no byte moved for 60 s fails), so a large upload on a slow link can finish. The response body
   * is read up to a small cap. `progress` is called as bytes go out.
   */
  static Roe<HttpResponse> PutFile(const std::string& url, const std::string& path,
                                   const std::map<std::string, std::string>& headers = {},
                                   const HttpProgress& progress = {});
  /**
   * GET into `dest_path`, written as it arrives. Hard cap: if more than `max_bytes` arrive the transfer is aborted
   * and the file removed, whatever the server said it would send. A non-2xx answer writes nothing (the file is
   * removed) and returns the status. Any failure leaves no file behind. Stall limit as for PutFile.
   */
  static Roe<HttpFileDownload> GetToFile(const std::string& url, const std::string& dest_path, uint64_t max_bytes,
                                         const HttpProgress& progress = {}, bool restrict_to_public_https = false,
                                         const std::map<std::string, std::string>& headers = {});

  /**
   * Pass std::nullopt only when a caller deliberately requires an unbounded response.
   *
   * `restrict_to_public_https`: for URLs sourced from a remote peer (attachment/profile-icon
   * fetch) rather than our own backend config — forces https (initial request and redirects)
   * and rejects any resolved address that is not a public-routable host (loopback/private/
   * link-local/CGNAT/multicast), checked after DNS resolution to also cover DNS rebinding.
   */
  static Roe<HttpResponse> Get(const std::string& url, const std::map<std::string, std::string>& headers = {},
                               std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                               HttpTimeout timeout = {}, bool restrict_to_public_https = false);
  static Roe<HttpResponse> Post(const std::string& url, const std::string& body,
                                const std::map<std::string, std::string>& headers = {},
                                std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                                HttpTimeout timeout = {}, bool restrict_to_public_https = false);
  static Roe<HttpResponse> Put(const std::string& url, const std::string& body,
                               const std::map<std::string, std::string>& headers = {},
                               std::optional<size_t> max_response_bytes = kMaxHttpClientBodyBytes,
                               HttpTimeout timeout = {}, bool restrict_to_public_https = false);
};

} // namespace pbr
