#include "domain/attachments/RelayFileUrl.h"

#include <array>
#include <cctype>
#include <string_view>

namespace pbr {

namespace {

/** The path the relay's download route sits under; the current family first, then a link sent before it. */
constexpr std::string_view kRelayV1 = "/api/relay/v1/";
constexpr std::array<std::string_view, 2> kFamilies = {"blob/", "files/"};
constexpr std::string_view kDownload = "/download";

bool IsBlobId(const std::string_view id) {
  if (id.empty() || id.size() > 64) {
    return false;
  }
  for (const char c : id) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
      return false;
    }
  }
  return true;
}

} // namespace

std::string RelayFileUrl::BlobIdOf(const std::string& url) {
  const std::string path = Redacted(url);
  const size_t scheme = path.find("://");
  if (scheme == std::string::npos) {
    return {};
  }
  // The route starts the path: right after the authority.
  const size_t route = path.find('/', scheme + 3);
  if (route == std::string::npos || path.compare(route, kRelayV1.size(), kRelayV1) != 0) {
    return {};
  }
  const std::string_view rest = std::string_view(path).substr(route + kRelayV1.size());
  for (const std::string_view family : kFamilies) {
    if (rest.substr(0, family.size()) != family) {
      continue;
    }
    const std::string_view tail = rest.substr(family.size());
    const size_t id_end = tail.find('/');
    if (id_end == std::string_view::npos || tail.substr(id_end) != kDownload) {
      return {};
    }
    const std::string_view id = tail.substr(0, id_end);
    return IsBlobId(id) ? std::string(id) : std::string();
  }
  return {};
}

std::string RelayFileUrl::Redacted(const std::string& url) {
  return url.substr(0, url.find_first_of("?#"));
}

} // namespace pbr
