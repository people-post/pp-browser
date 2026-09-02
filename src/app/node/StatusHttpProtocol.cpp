#include "app/node/StatusHttpProtocol.h"

#include "common/ValueJson.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::string ToLowerAscii(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool AuthOk(const StatusHttpAuthConfig& auth, std::string_view authorization) {
  if (auth.bearer_token.empty()) {
    return true;
  }
  const std::string prefix = "Bearer ";
  if (authorization.size() < prefix.size() ||
      !EqualsIgnoreCase(authorization.substr(0, prefix.size()), prefix)) {
    return false;
  }
  return authorization.substr(prefix.size()) == auth.bearer_token;
}

StatusHttpResponse JsonResponse(int code, const Object& body) {
  StatusHttpResponse r;
  r.status_code = code;
  r.content_type = "application/json";
  r.body = DumpJson(body);
  return r;
}

StatusHttpResponse Unauthorized() {
  Object body;
  body.set("error", "unauthorized");
  return JsonResponse(401, body);
}

StatusHttpResponse NotFound() {
  Object body;
  body.set("error", "not_found");
  return JsonResponse(404, body);
}

StatusHttpResponse MethodNotAllowed() {
  Object body;
  body.set("error", "method_not_allowed");
  return JsonResponse(405, body);
}

const char* StatusReason(int code) {
  switch (code) {
  case 200:
    return "OK";
  case 401:
    return "Unauthorized";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  default:
    return "Error";
  }
}

} // namespace

std::optional<StatusHttpBind> ParseStatusHttpBind(std::string_view spec) {
  const auto trimmed = Trim(spec);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  StatusHttpBind bind;
  std::string_view host_port = trimmed;

  if (!host_port.empty() && host_port.front() == '[') {
    const auto close = host_port.find(']');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    bind.host = std::string(host_port.substr(1, close - 1));
    host_port.remove_prefix(close + 1);
    if (host_port.empty() || host_port.front() != ':') {
      return std::nullopt;
    }
    host_port.remove_prefix(1);
  } else {
    const auto colon = host_port.rfind(':');
    if (colon == std::string_view::npos) {
      // Bare port
      uint16_t port = 0;
      const auto* begin = host_port.data();
      const auto* end = host_port.data() + host_port.size();
      if (std::from_chars(begin, end, port).ec != std::errc{} || port == 0) {
        return std::nullopt;
      }
      bind.host = kDefaultStatusHttpBindHost;
      bind.port = port;
      return bind;
    }
    bind.host = std::string(host_port.substr(0, colon));
    host_port.remove_prefix(colon + 1);
  }

  if (bind.host.empty() || host_port.empty()) {
    return std::nullopt;
  }
  uint16_t port = 0;
  const auto* begin = host_port.data();
  const auto* end = host_port.data() + host_port.size();
  if (std::from_chars(begin, end, port).ec != std::errc{} || port == 0) {
    return std::nullopt;
  }
  bind.port = port;
  return bind;
}

bool IsLoopbackStatusBindHost(std::string_view host) {
  const std::string lower = ToLowerAscii(Trim(host));
  return lower == "127.0.0.1" || lower == "::1" || lower == "localhost";
}

std::optional<StatusHttpRequest> TryParseStatusHttpRequest(std::string_view raw) {
  const auto header_end = raw.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view headers = raw.substr(0, header_end);
  const auto line_end = headers.find("\r\n");
  const std::string_view request_line =
      line_end == std::string_view::npos ? headers : headers.substr(0, line_end);

  const auto sp1 = request_line.find(' ');
  if (sp1 == std::string_view::npos) {
    return std::nullopt;
  }
  const auto sp2 = request_line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos) {
    return std::nullopt;
  }

  StatusHttpRequest req;
  req.method = std::string(request_line.substr(0, sp1));
  req.path = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
  // Strip query string
  if (const auto q = req.path.find('?'); q != std::string::npos) {
    req.path.resize(q);
  }

  if (line_end != std::string_view::npos) {
    std::string_view rest = headers.substr(line_end + 2);
    while (!rest.empty()) {
      const auto nl = rest.find("\r\n");
      const std::string_view line = nl == std::string_view::npos ? rest : rest.substr(0, nl);
      const auto colon = line.find(':');
      if (colon != std::string_view::npos) {
        const auto name = Trim(line.substr(0, colon));
        const auto value = Trim(line.substr(colon + 1));
        if (EqualsIgnoreCase(name, "Authorization")) {
          req.authorization = std::string(value);
        }
      }
      if (nl == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(nl + 2);
    }
  }
  return req;
}

StatusHttpResponse HandleStatusHttpRequest(const StatusHttpRequest& request,
                                           const StatusHttpAuthConfig& auth,
                                           const StatusHttpSnapshot& snap) {
  if (!AuthOk(auth, request.authorization)) {
    return Unauthorized();
  }
  if (request.method != "GET" && request.method != "HEAD") {
    return MethodNotAllowed();
  }

  if (request.path == "/healthz") {
    Object body;
    body.set("ok", snap.host_running);
    body.set("host_running", snap.host_running);
    return JsonResponse(200, body);
  }

  if (request.path == "/status") {
    Object reach;
    if (!snap.reachability_json.empty()) {
      if (auto parsed = TryParseObject(snap.reachability_json)) {
        reach = std::move(*parsed);
      }
    }
    reach.set("host_running", snap.host_running);
    if (!snap.listen_multiaddr.empty()) {
      reach.set("listen", snap.listen_multiaddr);
    }
    if (!snap.peer_id.empty()) {
      reach.set("peer_id", snap.peer_id);
    }
    reach.set("circuit_relay", snap.circuit_relay);
    reach.set("media_relay", snap.media_relay);
    reach.set("dht", snap.dht);
    if (!snap.dht_json.empty()) {
      if (auto dht = TryParseObject(snap.dht_json)) {
        reach.set("dht_stats", *dht);
      }
    }
    return JsonResponse(200, reach);
  }

  return NotFound();
}

std::string FormatStatusHttpResponse(const StatusHttpResponse& response) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status_code << ' ' << StatusReason(response.status_code) << "\r\n"
      << "Content-Type: " << response.content_type << "\r\n"
      << "Content-Length: " << response.body.size() << "\r\n"
      << "Connection: close\r\n"
      << "Cache-Control: no-store\r\n"
      << "\r\n"
      << response.body;
  return out.str();
}

} // namespace pbr
