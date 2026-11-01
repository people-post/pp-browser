#include "foundation/platform/PickedFileName.h"

#include <cctype>

namespace pbr {

namespace {

std::string PercentDecode(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
      out.push_back(static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

} // namespace

std::string DisplayNameFromPickedPath(const std::string& path_or_uri) {
  std::string name = path_or_uri;
  const size_t query = name.find_first_of("?#");
  if (query != std::string::npos) {
    name.resize(query);
  }
  name = PercentDecode(name);
  const size_t cut = name.find_last_of("/\\:");
  if (cut != std::string::npos) {
    name.erase(0, cut + 1);
  }
  const size_t dot = name.find_last_of('.');
  if (name.empty() || dot == std::string::npos || dot == 0 || name.size() - dot - 1 < 1 || name.size() - dot - 1 > 8) {
    return {};
  }
  return name;
}

} // namespace pbr
