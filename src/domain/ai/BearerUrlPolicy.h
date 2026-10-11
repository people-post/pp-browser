#pragma once

#include <string_view>

namespace pbr {

// A bearer key travels only over https, or over plain http to this machine (PB-035). The host is matched
// whole: "http://localhost.example.com" is not local.
inline bool BearerAllowedOnUrl(std::string_view url) {
  if (url.rfind("https://", 0) == 0) {
    return true;
  }
  for (const std::string_view local : {"http://127.0.0.1", "http://localhost", "http://[::1]"}) {
    if (url.rfind(local, 0) != 0) {
      continue;
    }
    const std::string_view rest = url.substr(local.size());
    if (rest.empty() || rest.front() == '/' || rest.front() == ':') {
      return true;
    }
  }
  return false;
}

} // namespace pbr
