#include "foundation/platform/ClipboardFiles.h"

#include <cctype>
#include <utility>

namespace pbr {
namespace {

int HexValue(const char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/** false = a '%' not followed by two hex digits. */
bool PercentDecode(const std::string_view text, std::string& out) {
  out.clear();
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size()) {
      return false;
    }
    const int high = HexValue(text[i + 1]);
    const int low = HexValue(text[i + 2]);
    if (high < 0 || low < 0) {
      return false;
    }
    out.push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }
  return true;
}

bool EqualsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

} // namespace

std::vector<std::string> ParseUriListFiles(const std::string_view uri_list) {
  std::vector<std::string> files;
  size_t pos = 0;
  while (pos <= uri_list.size()) {
    size_t end = uri_list.find_first_of("\r\n", pos);
    if (end == std::string_view::npos) {
      end = uri_list.size();
    }
    std::string_view line = uri_list.substr(pos, end - pos);
    pos = end + 1;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      line.remove_suffix(1);
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    constexpr std::string_view kScheme = "file://";
    if (line.size() < kScheme.size() || !EqualsIgnoreCase(line.substr(0, kScheme.size()), kScheme)) {
      continue;
    }
    std::string_view rest = line.substr(kScheme.size());
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
      continue; // no path
    }
    const std::string_view host = rest.substr(0, slash);
    if (!host.empty() && !EqualsIgnoreCase(host, "localhost")) {
      continue; // another machine
    }
    std::string path;
    if (!PercentDecode(rest.substr(slash), path) || path.size() < 2 || path.find('\0') != std::string::npos) {
      continue;
    }
    files.push_back(std::move(path));
  }
  return files;
}

} // namespace pbr
