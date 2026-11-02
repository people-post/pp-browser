#include "foundation/diagnostics/DiagnosticsSelfCheck.h"

#include <array>
#include <cctype>
#include <cstddef>

// Hand-written scanners instead of std::regex: log lines can be thousands of bytes long, and the
// recursive std::regex implementations (MSVC, libstdc++) overflow the stack on long greedy runs.

namespace pbr {

namespace {

bool IsAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsB64Url(char c) { return IsAlnum(c) || c == '-' || c == '_'; }

constexpr std::array<std::string_view, 7> kPathRoots = {
    "/Users/", "/home/", "/var/mobile/", "/private/var/", "/data/user/", "/data/data/", "/storage/"};

bool StartsWith(std::string_view s, std::size_t pos, std::string_view prefix) {
  return s.size() - pos >= prefix.size() && s.compare(pos, prefix.size(), prefix) == 0;
}

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
    if (!IsB64Url(body[i])) { ++i; continue; }
    std::size_t e = i;
    int digits = 0;
    bool upper = false;
    bool lower = false;
    while (e < body.size() && IsB64Url(body[e])) {
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
  // Most specific first: a known id is also a long token.
  for (const std::string& raw : known) {
    std::string_view id = raw;
    for (const std::string_view scheme : {"account:", "relay:", "peer:", "call:"}) {
      if (id.substr(0, scheme.size()) == scheme) {
        id.remove_prefix(scheme.size());
        break;
      }
    }
    if (id.size() < 8) continue;  // too short to match without false positives
    // Any prefix of 8+ chars is a prefix of the first 8, so searching those 8 covers the id and its prefixes.
    if (text.find(id.substr(0, 8)) != std::string_view::npos) return std::string("known_identifier");
  }
  for (auto b : bodies) if (HasHexRun(b)) return std::string("hex_run");
  for (auto b : bodies) if (HasLongToken(b)) return std::string("long_token");
  for (auto b : bodies) if (HasEmail(b)) return std::string("email");
  for (auto b : bodies) if (HasAbsPath(b)) return std::string("abs_path");
  return std::nullopt;
}

}  // namespace pbr
