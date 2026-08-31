#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** Loopback ops admin bind (default). Not the libp2p listen port. */
inline constexpr const char* kDefaultStatusHttpBindHost = "127.0.0.1";
inline constexpr uint16_t kDefaultStatusHttpBindPort = 18518;

struct StatusHttpBind {
  std::string host = kDefaultStatusHttpBindHost;
  uint16_t port = kDefaultStatusHttpBindPort;
};

/**
 * Parse `host:port` (IPv4 or hostname). Empty string → disabled (nullopt).
 * Bare port (`18518`) means 127.0.0.1:port. `[::1]:port` supported for IPv6 loopback.
 */
std::optional<StatusHttpBind> ParseStatusHttpBind(std::string_view spec);

/** True for 127.0.0.1, ::1, localhost (case-insensitive). */
bool IsLoopbackStatusBindHost(std::string_view host);

struct StatusHttpAuthConfig {
  /** Empty = no auth required. */
  std::string bearer_token;
};

struct StatusHttpSnapshot {
  bool host_running = false;
  std::string listen_multiaddr;
  std::string peer_id;
  bool circuit_relay = false;
  bool media_relay = false;
  /** Object JSON from ReachabilityService::FormatOpsStatusJson() (or "{}"). */
  std::string reachability_json = "{}";
};

struct StatusHttpResponse {
  int status_code = 200;
  std::string content_type = "application/json";
  std::string body;
};

/** Parse request-line + headers blob (up to and including header terminator). */
struct StatusHttpRequest {
  std::string method;
  std::string path;
  std::string authorization;
};

/** Returns nullopt if the buffer is incomplete or malformed beyond recovery. */
std::optional<StatusHttpRequest> TryParseStatusHttpRequest(std::string_view raw);

StatusHttpResponse HandleStatusHttpRequest(const StatusHttpRequest& request,
                                           const StatusHttpAuthConfig& auth,
                                           const StatusHttpSnapshot& snap);

/** Serialize a complete HTTP/1.1 response. */
std::string FormatStatusHttpResponse(const StatusHttpResponse& response);

} // namespace pbr
