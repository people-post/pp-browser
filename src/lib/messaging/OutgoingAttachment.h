#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/** An attachment ready to send: the bytes that will be encrypted and uploaded, and how to name them. */
struct OutgoingAttachment {
  std::string filename;
  std::string mime;
  std::vector<uint8_t> bytes;
  /** A picture that went through the automatic shrink (its limit is AttachmentLimits::max_image_bytes). */
  bool image_is_shrunk = false;
};

} // namespace pbr
