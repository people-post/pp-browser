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

/**
 * Whether `url` can be offered as an update link: `https://`, a host name, no whitespace, and not a reserved test /
 * example domain (RFC 2606: `.test`, `.example`, `.invalid`, `.localhost`, `example.com / .net / .org` and their
 * subdomains), which is what the relay's placeholder link looks like.
 */
inline bool IsUsableUpdateUrl(std::string_view url) {
  using namespace update_link_detail;
  constexpr std::string_view kScheme = "https://";
  if (url.size() <= kScheme.size() || Lower(url.substr(0, kScheme.size())) != kScheme) {
    return false;
  }
  if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return std::isspace(c) != 0; })) {
    return false;
  }
  std::string_view rest = url.substr(kScheme.size());
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const size_t at = rest.rfind('@'); at != std::string_view::npos) {
    rest = rest.substr(at + 1);
  }
  if (const size_t colon = rest.find(':'); colon != std::string_view::npos) {
    rest = rest.substr(0, colon);
  }
  std::string host = Lower(rest);
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  if (host.empty()) {
    return false;
  }
  for (const std::string_view reserved :
       {"test", "example", "invalid", "localhost", "example.com", "example.net", "example.org"}) {
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
  const auto offered = [&](const std::string& url) { return IsUsableUpdateUrl(url) && (url != fallback || desktop); };
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
