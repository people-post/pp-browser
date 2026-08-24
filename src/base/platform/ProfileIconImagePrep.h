#pragma once

#include "common/Error.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pbr {

struct EncodedProfileIcon {
  std::vector<uint8_t> bytes;
  std::string content_type;
  std::string kind;
  std::string file_extension;
};

/** Load, scale, and encode an image file for relay profile icon upload. */
Roe<EncodedProfileIcon> PrepareProfileIconFromFile(const std::string& path,
                                                    size_t max_bytes = 512u * 1024u,
                                                    int max_dimension = 256);

} // namespace pbr
