#include "foundation/diagnostics/DiagnosticsScrub.h"

#include <array>
#include <cctype>
#include <algorithm>
#include <cstddef>

// Hand-written scanners instead of std::regex: log lines can be thousands of bytes long, and the
// recursive std::regex implementations (MSVC, libstdc++) overflow the stack on long greedy runs.

namespace pbr {

namespace {

bool IsAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsB64Url(char c) { return IsAlnum(c) || c == '-' || c == '_'; }
bool IsSpaceOrQuote(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == '"' || c == '\'';
}
bool IsBase58(char c) {
  return IsAlnum(c) && c != '0' && c != 'O' && c != 'I' && c != 'l';
}

constexpr std::array<std::string_view, 7> kPathRoots = {
    "/Users/", "/home/", "/var/mobile/", "/private/var/", "/data/user/", "/data/data/", "/storage/"};

bool StartsWith(std::string_view s, std::size_t pos, std::string_view prefix) {
  return s.size() - pos >= prefix.size() && s.compare(pos, prefix.size(), prefix) == 0;
}

std::string Placeholder(ScrubState& st, const char* kind, const std::string& text) {
  const std::string key = std::string(kind) + "|" + text;
  auto it = st.assigned.find(key);
  if (it == st.assigned.end()) {
    const int n = ++st.counters[kind];
    it = st.assigned.emplace(key, n).first;
  }
  return std::string(kind) + "_" + std::to_string(it->second);
}

/** Length of an optional ":<1-5 digits>" port at pos that is not followed by another alnum; 0 if none. */
std::size_t PortLen(std::string_view s, std::size_t pos) {
  if (pos >= s.size() || s[pos] != ':') return 0;
  std::size_t j = pos + 1;
  while (j < s.size() && IsDigit(s[j])) ++j;
  const std::size_t digits = j - pos - 1;
  if (digits < 1 || digits > 5) return 0;
  if (j < s.size() && IsAlnum(s[j])) return 0;
  return j - pos;
}

bool ValidIpv6(std::string_view t) {
  // t is made of hex digits, ':' and '.' (optionally an IPv4 tail). A timestamp such as 12:30:45 or
  // 08:57:32.329 must not pass: require "::" or a full-length form.
  int colons = 0;
  bool double_colon = false;
  bool digit = false;
  bool dot = false;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i] == ':') {
      ++colons;
      if (i + 1 < t.size() && t[i + 1] == ':') double_colon = true;
    } else if (t[i] == '.') {
      dot = true;
    } else if (IsDigit(t[i])) {
      digit = true;
    }
  }
  if (colons < 2 || !digit) return false;
  if (double_colon) return true;
  return colons == 7 || (dot && colons == 6);
}

bool IsFileLikeTld(std::string tld) {
  for (char& c : tld) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const std::array<std::string_view, 20> kFileTlds = {
      "c", "cc", "cpp", "cxx", "h", "hpp", "hh", "m", "mm", "rs", "py", "js", "ts", "java", "kt", "swift",
      "txt", "log", "json", "rml"};
  for (const auto& t : kFileTlds) {
    if (tld == t) return true;
  }
  return false;
}


/** A matcher either consumes `len` bytes at the cursor and appends a placeholder, or returns 0. */
std::size_t MatchPrefixed(std::string_view s, std::size_t i, ScrubState& st, std::string& out) {
  const std::size_t n = s.size();
  const char c = s[i];
  if (c == 'a' && StartsWith(s, i, "account:")) {
    const std::size_t b = i + 8;
    std::size_t e = b;
    while (e < n && IsB64Url(s[e])) ++e;
    if (e - b == 43) {
      out += Placeholder(st, "acct", std::string(s.substr(b, 43)));
      return e - i;
    }
  } else if (c == 'r' && StartsWith(s, i, "relay:")) {
    const std::size_t b = i + 6;
    std::size_t e = b;
    while (e < n && e - b < 64 && (IsAlnum(s[e]) || s[e] == '_' || s[e] == '.' || s[e] == '-')) ++e;
    if (e > b) {
      out += Placeholder(st, "relay", std::string(s.substr(b, e - b)));
      return e - i;
    }
  } else if (c == 'c' && StartsWith(s, i, "call:")) {
    const std::size_t b = i + 5;
    std::size_t e = b;
    while (e < n && IsHex(s[e])) ++e;
    if (e - b == 32 && !(e < n && IsAlnum(s[e]))) {
      out += Placeholder(st, "call", std::string(s.substr(b, 32)));
      return e - i;
    }
  }
  return 0;
}

std::size_t MatchPath(std::string_view s, std::size_t i, ScrubState& st, std::string& out) {
  const std::size_t n = s.size();
  if (i > 0 && IsAlnum(s[i - 1])) return 0;
  bool is_path = false;
  if (s[i] == '/') {
    for (const auto& root : kPathRoots) {
      if (StartsWith(s, i, root)) {
        is_path = true;
        break;
      }
    }
  } else if (std::isalpha(static_cast<unsigned char>(s[i])) && i + 1 < n && s[i + 1] == ':') {
    // X:\Users\..., X:\\Users\\..., X:/Users/...
    std::size_t j = i + 2;
    std::size_t seps = 0;
    while (j < n && (s[j] == '\\' || s[j] == '/') && seps < 2) { ++j; ++seps; }
    if (seps > 0 && StartsWith(s, j, "Users")) {
      j += 5;
      is_path = j < n && (s[j] == '\\' || s[j] == '/');
    }
  }
  if (!is_path) return 0;
  std::size_t e = i;
  while (e < n && !IsSpaceOrQuote(s[e])) ++e;
  out += Placeholder(st, "path", std::string(s.substr(i, e - i)));
  return e - i;
}

std::size_t MatchMultiaddr(std::string_view s, std::size_t i, ScrubState& st, std::string& out) {
  static constexpr std::array<std::string_view, 5> kKinds = {"/ip4/", "/ip6/", "/dns/", "/dns4/", "/dns6/"};
  const std::size_t n = s.size();
  for (const auto& kind : kKinds) {
    if (!StartsWith(s, i, kind)) continue;
    const std::size_t b = i + kind.size();
    std::size_t e = b;
    while (e < n && s[e] != '/' && !IsSpaceOrQuote(s[e])) ++e;
    if (e == b) return 0;
    std::size_t t = e;
    for (std::string_view proto : {std::string_view("/tcp/"), std::string_view("/udp/")}) {
      if (StartsWith(s, t, proto)) {
        const std::size_t d = t + proto.size();
        std::size_t de = d;
        while (de < n && IsDigit(s[de])) ++de;
        if (de > d) t = de;
        break;
      }
    }
    out += Placeholder(st, "ip", std::string(s.substr(b, e - b)));
    return t - i;
  }
  return 0;
}

std::size_t MatchBracketedIpv6(std::string_view s, std::size_t i, ScrubState& st, std::string& out) {
  const std::size_t n = s.size();
  std::size_t e = i + 1;
  while (e < n && e - i <= 50 && (IsHex(s[e]) || s[e] == ':' || s[e] == '.')) ++e;
  if (e >= n || s[e] != ']' || e == i + 1) return 0;
  const std::string_view inner = s.substr(i + 1, e - i - 1);
  if (!ValidIpv6(inner)) return 0;
  out += Placeholder(st, "ip", std::string(inner));
  return e + 1 + PortLen(s, e + 1) - i;
}

std::size_t MatchIpv4(std::string_view s, std::size_t i, ScrubState& st, std::string& out) {
  const std::size_t n = s.size();
  if (i > 0 && (IsAlnum(s[i - 1]) || s[i - 1] == '.')) return 0;
  std::size_t j = i;
  for (int octet = 0; octet < 4; ++octet) {
    std::size_t d = j;
    int value = 0;
    while (d < n && IsDigit(s[d]) && d - j < 3) {
      value = value * 10 + (s[d] - '0');
      ++d;
    }
    if (d == j || value > 255) return 0;
    j = d;
    if (octet < 3) {
      if (j >= n || s[j] != '.') return 0;
      ++j;
    }
  }
  if (j < n && (IsDigit(s[j]) || (s[j] == '.' && j + 1 < n && IsDigit(s[j + 1])))) return 0;
  const std::string ip(s.substr(i, j - i));
  const std::size_t port = PortLen(s, j);
  out += Placeholder(st, "ip", ip);
  return j + port - i;
}

std::size_t MatchHostPort(std::string_view s, std::size_t i, ScrubState& st, std::string& out) {
  const std::size_t n = s.size();
  if (!IsAlnum(s[i])) return 0;
  if (i > 0 && (IsAlnum(s[i - 1]) || s[i - 1] == '.' || s[i - 1] == '-')) return 0;
  std::size_t e = i;
  while (e < n && (IsAlnum(s[e]) || s[e] == '.' || s[e] == '-')) ++e;
  const std::size_t port = PortLen(s, e);
  if (port == 0) return 0;
  const std::string host(s.substr(i, e - i));
  if (host.back() == '.' || host.back() == '-') return 0;
  if (host != "localhost") {
    const std::size_t dot = host.rfind('.');
    if (dot == std::string::npos) return 0;
    const std::string tld = host.substr(dot + 1);
    if (tld.size() < 2) return 0;
    for (char ch : tld) {
      if (!std::isalpha(static_cast<unsigned char>(ch))) return 0;
    }
    if (IsFileLikeTld(tld)) return 0;
  }
  std::string key = host;
  for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  out += Placeholder(st, "ip", key);
  return e + port - i;
}

std::size_t MatchBareIpv6(std::string_view s, std::size_t i, ScrubState& st, std::string& out) {
  const std::size_t n = s.size();
  if (!(IsHex(s[i]) || (s[i] == ':' && i + 1 < n && s[i + 1] == ':'))) return 0;
  if (i > 0 && (IsAlnum(s[i - 1]) || s[i - 1] == ':' || s[i - 1] == '.')) return 0;
  std::size_t e = i;
  while (e < n && (IsHex(s[e]) || s[e] == ':' || s[e] == '.')) ++e;
  while (e > i && s[e - 1] == '.') --e;  // sentence-ending dot
  if (e < n && IsAlnum(s[e])) return 0;
  const std::string_view tok = s.substr(i, e - i);
  if (!ValidIpv6(tok)) return 0;
  std::size_t end = e;
  if (end < n && s[end] == '%') {  // zone id
    ++end;
    while (end < n && IsAlnum(s[end])) ++end;
  }
  out += Placeholder(st, "ip", std::string(tok));
  return end - i;
}

std::size_t MatchPeerId(std::string_view s, std::size_t i, ScrubState& st, std::string& out) {
  const std::size_t n = s.size();
  if (!IsAlnum(s[i]) || (i > 0 && IsAlnum(s[i - 1]))) return 0;
  std::size_t e = i;
  while (e < n && IsAlnum(s[e])) ++e;
  const std::size_t len = e - i;
  if (len < 32 || len > 50) return 0;
  for (std::size_t k = i; k < e; ++k) {
    if (!IsBase58(s[k])) return 0;
  }
  out += Placeholder(st, "peer", std::string(s.substr(i, len)));
  return len;
}

}  // namespace

std::string ScrubLine(std::string_view s, ScrubState& st) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t used = MatchPrefixed(s, i, st, out);
    if (used == 0) used = MatchPath(s, i, st, out);
    if (used == 0 && s[i] == '/') used = MatchMultiaddr(s, i, st, out);
    if (used == 0 && s[i] == '[') used = MatchBracketedIpv6(s, i, st, out);
    if (used == 0 && IsDigit(s[i])) used = MatchIpv4(s, i, st, out);
    if (used == 0) used = MatchHostPort(s, i, st, out);
    if (used == 0) used = MatchBareIpv6(s, i, st, out);
    if (used == 0) used = MatchPeerId(s, i, st, out);
    if (used == 0) {
      out.push_back(s[i]);
      ++i;
    } else {
      i += used;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------
// Self-check: independent of the matchers above.

namespace {

std::string_view BodyOf(std::string_view line) {
  // Skip a leading "[ts] [LEVEL] [Channel] " so the time stamp and channel never count as findings.
  std::size_t pos = 0;
  int groups = 0;
  while (groups < 3 && pos < line.size() && line[pos] == '[') {
    const std::size_t close = line.find(']', pos);
    if (close == std::string_view::npos) break;
    pos = close + 1;
    if (pos < line.size() && line[pos] == ' ') ++pos;
    ++groups;
  }
  return groups == 3 ? line.substr(pos) : line;
}

bool IsPlaceholderAt(std::string_view s, std::size_t i, std::size_t* len) {
  static constexpr std::array<std::string_view, 6> kKinds = {"acct_", "peer_", "relay_", "call_", "ip_", "path_"};
  if (i > 0 && IsAlnum(s[i - 1])) return false;
  for (const auto& k : kKinds) {
    if (!StartsWith(s, i, k)) continue;
    std::size_t j = i + k.size();
    const std::size_t d = j;
    while (j < s.size() && IsDigit(s[j])) ++j;
    if (j > d) {
      *len = j - i;
      return true;
    }
  }
  return false;
}

bool HasHexRun(std::string_view body) {
  std::size_t i = 0;
  while (i < body.size()) {
    if (!IsHex(body[i])) { ++i; continue; }
    std::size_t e = i;
    bool letter = false;
    while (e < body.size() && IsHex(body[e])) {
      if (!IsDigit(body[e])) letter = true;
      ++e;
    }
    const bool after_0x = i >= 2 && body[i - 2] == '0' && (body[i - 1] == 'x' || body[i - 1] == 'X');
    if (e - i >= 16 && letter && !after_0x) return true;
    i = e;
  }
  return false;
}

bool HasLongToken(std::string_view body) {
  std::size_t i = 0;
  while (i < body.size()) {
    std::size_t plen = 0;
    if (IsPlaceholderAt(body, i, &plen)) { i += plen; continue; }
    if (!IsB64Url(body[i])) { ++i; continue; }
    std::size_t e = i;
    int digits = 0;
    bool upper = false;
    bool lower = false;
    while (e < body.size() && IsB64Url(body[e])) {
      std::size_t pl = 0;
      if (IsPlaceholderAt(body, e, &pl) && e > i) break;
      if (IsDigit(body[e])) ++digits;
      else if (body[e] >= 'A' && body[e] <= 'Z') upper = true;
      else if (body[e] >= 'a' && body[e] <= 'z') lower = true;
      ++e;
    }
    // Id-like: mixed case and at least 5 digits. Class / enum / snake_case names carry one or two digits.
    if (e - i >= 20 && upper && lower && digits >= 5) return true;
    i = e;
  }
  return false;
}

bool HasEmail(std::string_view body) {
  for (std::size_t at = body.find('@'); at != std::string_view::npos; at = body.find('@', at + 1)) {
    std::size_t l = at;
    while (l > 0 && (IsAlnum(body[l - 1]) || std::string_view("._%+-").find(body[l - 1]) != std::string_view::npos)) --l;
    if (l == at) continue;
    std::size_t r = at + 1;
    while (r < body.size() && (IsAlnum(body[r]) || body[r] == '.' || body[r] == '-')) ++r;
    while (r > at + 1 && body[r - 1] == '.') --r;
    const std::string_view domain = body.substr(at + 1, r - at - 1);
    const std::size_t dot = domain.rfind('.');
    if (dot == std::string_view::npos || domain.size() - dot - 1 < 2) continue;
    bool alpha = true;
    for (std::size_t k = dot + 1; k < domain.size(); ++k) {
      if (!std::isalpha(static_cast<unsigned char>(domain[k]))) alpha = false;
    }
    if (alpha) return true;
  }
  return false;
}

bool HasAbsPath(std::string_view body) {
  for (const auto& root : kPathRoots) {
    for (std::size_t p = body.find(root); p != std::string_view::npos; p = body.find(root, p + 1)) {
      if (p == 0 || !IsAlnum(body[p - 1])) return true;
    }
  }
  for (std::size_t p = body.find(":\\"); p != std::string_view::npos; p = body.find(":\\", p + 1)) {
    if (p >= 1 && std::isalpha(static_cast<unsigned char>(body[p - 1])) && (p == 1 || !IsAlnum(body[p - 2]))) {
      std::size_t j = p + 1;
      while (j < body.size() && body[j] == '\\') ++j;
      if (StartsWith(body, j, "Users")) return true;
    }
  }
  for (std::size_t p = body.find(":/Users/"); p != std::string_view::npos; p = body.find(":/Users/", p + 1)) {
    if (p >= 1 && std::isalpha(static_cast<unsigned char>(body[p - 1])) && (p == 1 || !IsAlnum(body[p - 2]))) return true;
  }
  return false;
}

}  // namespace

std::optional<std::string> SelfCheck(std::string_view text, const std::vector<std::string>& known) {
  std::vector<std::string_view> bodies;
  for (std::size_t pos = 0; pos <= text.size();) {
    std::size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    bodies.push_back(BodyOf(text.substr(pos, nl - pos)));
    pos = nl + 1;
  }
  for (auto b : bodies) if (HasHexRun(b)) return std::string("hex_run");
  for (auto b : bodies) if (HasLongToken(b)) return std::string("long_token");
  for (auto b : bodies) if (HasEmail(b)) return std::string("email");
  for (auto b : bodies) if (HasAbsPath(b)) return std::string("abs_path");
  for (const std::string& id : known) {
    if (id.empty()) continue;
    const std::string_view needle = std::string_view(id).substr(0, std::min<std::size_t>(id.size(), 8));
    if (text.find(needle) != std::string_view::npos) return std::string("known_identifier");
  }
  return std::nullopt;
}

}  // namespace pbr
