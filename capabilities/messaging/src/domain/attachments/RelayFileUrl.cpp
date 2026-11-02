#include "domain/attachments/RelayFileUrl.h"

#include <cctype>
#include <string_view>

namespace pbr {

std::string RelayFileUrl::BlobIdOf(const std::string& url) {
  constexpr std::string_view kFiles = "/api/relay/v1/files/";
  constexpr std::string_view kDownload = "/download";
  const std::string path = Redacted(url);
  const size_t scheme = path.find("://");
  const size_t files = path.find(kFiles, scheme == std::string::npos ? 0 : scheme + 3);
  if (scheme == std::string::npos || files == std::string::npos || path.find('/', scheme + 3) != files) {
    return {};
  }
  const size_t id_start = files + kFiles.size();
  const size_t id_end = path.find('/', id_start);
  if (id_end == std::string::npos || path.compare(id_end, std::string::npos, kDownload) != 0) {
    return {};
  }
  const std::string id = path.substr(id_start, id_end - id_start);
  if (id.empty() || id.size() > 64) {
    return {};
  }
  for (const char c : id) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
      return {};
    }
  }
  return id;
}

std::string RelayFileUrl::Redacted(const std::string& url) {
  return url.substr(0, url.find_first_of("?#"));
}

} // namespace pbr
