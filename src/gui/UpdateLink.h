#pragma once

#include "lib/net/ClientCompat.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace pbr {

namespace update_link_detail {

inline std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

inline bool EndsWith(const std::string& text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/** `name` itself or any subdomain of it. */
inline bool IsOrUnder(const std::string& host, std::string_view name) {
  if (host == name) {
    return true;
  }
  return host.size() > name.size() && EndsWith(host, name) && host[host.size() - name.size() - 1] == '.';
}

} // namespace update_link_detail

/** The host of an https URL, lower case, without userinfo / port / trailing dots; empty when it has none. */
inline std::string HttpsHost(std::string_view url) {
  using namespace update_link_detail;
  constexpr std::string_view kScheme = "https://";
  if (url.size() <= kScheme.size() || Lower(url.substr(0, kScheme.size())) != kScheme) {
    return {};
  }
  std::string_view rest = url.substr(kScheme.size());
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const size_t at = rest.rfind('@'); at != std::string_view::npos) {
    rest = rest.substr(at + 1);
  }
  if (rest.empty() || rest.front() == '[') {
    return {}; // a bracketed (IPv6) host is not a web address a store or download page lives at
  }
  if (const size_t colon = rest.find(':'); colon != std::string_view::npos) {
    rest = rest.substr(0, colon);
  }
  std::string host = Lower(rest);
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  return host;
}

/** Scheme and host lower case, a trailing slash of the path ignored: two spellings of one link compare equal. */
inline std::string NormalisedLink(std::string_view url) {
  using namespace update_link_detail;
  constexpr std::string_view kScheme = "https://";
  if (url.size() < kScheme.size() || Lower(url.substr(0, kScheme.size())) != kScheme) {
    return std::string(url);
  }
  std::string_view rest = url.substr(kScheme.size());
  const size_t cut = rest.find_first_of("/?#");
  std::string out = std::string(kScheme) + Lower(rest.substr(0, cut));
  if (cut != std::string_view::npos) {
    out += std::string(rest.substr(cut));
  }
  while (out.size() > kScheme.size() && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

/**
 * Whether `url` can be offered as an update link: `https://`, no whitespace, and a real web host: letters, digits,
 * dots and hyphens only (punycode `xn--` labels are fine), at least two labels, not an IP address (dotted digits or a
 * bracketed IPv6), not `.local`, and not a reserved test / example domain (RFC 2606: `.test`, `.example`, `.invalid`,
 * `.localhost`, `example.com / .net / .org` and their subdomains), which is what the relay's placeholder link looks like.
 */
inline bool IsUsableUpdateUrl(std::string_view url) {
  using namespace update_link_detail;
  if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return std::isspace(c) != 0; })) {
    return false;
  }
  const std::string host = HttpsHost(url);
  if (host.empty() || host.find('.') == std::string::npos) {
    return false; // none, or a single label (localhost, an intranet name)
  }
  bool all_numeric = true;
  size_t label_start = 0;
  while (label_start <= host.size()) {
    size_t dot = host.find('.', label_start);
    if (dot == std::string::npos) {
      dot = host.size();
    }
    const std::string label = host.substr(label_start, dot - label_start);
    if (label.empty()) {
      return false;
    }
    if (!std::all_of(label.begin(), label.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
      all_numeric = false;
    }
    label_start = dot + 1;
  }
  if (all_numeric) {
    return false; // 192.168.0.1 and the like
  }
  if (!std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
      })) {
    return false;
  }
  for (const std::string_view reserved :
       {"local", "test", "example", "invalid", "localhost", "example.com", "example.net", "example.org"}) {
    if (IsOrUnder(host, reserved)) {
      return false;
    }
  }
  return true;
}

/**
 * The link the "new version" card's Update button opens, empty when there is none: the document's link for
 * `platform` (Platform::OsName()), else its generic `upgrade_url`, the first that passes IsUsableUpdateUrl; failing
 * both, the compiled `fallback` (the GitHub releases page) on macOS and Windows only, the platforms whose installers
 * are published there. iOS and Android install from the store or TestFlight and Linux has no published installer, so
 * no link beats a page that is not where the app comes from.
 */
inline std::string UsableUpdateLink(const ClientCompatDocument& doc, const std::string& platform,
                                    const std::string& fallback) {
  const bool desktop = platform == "macos" || platform == "windows";
  // A link the document gives that is the releases page itself is the fallback under another name: same rule.
  const std::string fallback_link = NormalisedLink(fallback);
  const auto offered = [&](const std::string& url) {
    return IsUsableUpdateUrl(url) && (NormalisedLink(url) != fallback_link || desktop);
  };
  if (const auto own = doc.upgrade_urls.find(platform); own != doc.upgrade_urls.end() && offered(own->second)) {
    return own->second;
  }
  if (offered(doc.upgrade_url)) {
    return doc.upgrade_url;
  }
  if (desktop && IsUsableUpdateUrl(fallback)) {
    return fallback;
  }
  return {};
}

} // namespace pbr
