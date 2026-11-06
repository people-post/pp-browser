#pragma once

#include <cctype>
#include <string>

namespace pbr {

/** The short type badge of a file ("PDF", "MP3"): its name's extension, else the mime's subtype; at most 4 letters. */
inline std::string AttachmentTypeLabel(const std::string& filename, const std::string& mime) {
  std::string ext;
  if (const size_t dot = filename.rfind('.'); dot != std::string::npos && dot + 1 < filename.size()) {
    ext = filename.substr(dot + 1);
  } else if (const size_t slash = mime.find('/'); slash != std::string::npos) {
    ext = mime.substr(slash + 1);
  }
  if (ext.size() > 4) {
    ext.resize(4);
  }
  for (char& c : ext) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return ext;
}

} // namespace pbr
