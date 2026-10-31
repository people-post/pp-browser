#pragma once

#include "common/Error.h"
#include "lib/messaging/AttachmentLimits.h"
#include "lib/messaging/OutgoingAttachment.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Why a picked file was not made ready; carried in Error::code. */
enum class OutgoingAttachmentError : int32_t {
  TooLarge = 1,   // over the limit for its kind (the Error's user text names the limit)
  Unreadable = 2, // cannot be opened or read
  Empty = 3,
  ImageFailed = 4, // a picture that decodes but cannot be shrunk into the limit
};

/** The most pixels a picture may have to be shrunk (AttachmentLimits::max_image_pixels); the longest edge afterwards. */
inline constexpr int kShrunkImageLongEdge = 2048;

/**
 * Make a picked file ready to send, off the UI thread. The size limit for its kind is checked before the file
 * is read. A PNG / JPEG / WebP picture is shrunk (long edge 2048, JPEG 85%, turned upright by its EXIF
 * orientation, no metadata) into `max_image_bytes`; `original` sends it as it is instead, but a JPEG loses its
 * location and camera data. A GIF, a picture that cannot be decoded, and every other kind go as plain files.
 */
Roe<OutgoingAttachment> PrepareOutgoingAttachment(const std::string& path, const AttachmentLimits& limits,
                                                  bool original);

} // namespace pbr
