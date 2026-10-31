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
  /** A picture that cannot be read or decoded (or whose location cannot be removed): it is not sent as a picture. */
  ImageUnprocessable = 5,
};

/** Which menu entry the file came from. */
enum class OutgoingPickSource {
  Photo,    // "Photo": a picture, always shrunk; one that cannot be processed is refused
  Original, // "Original photo": as it is, but a JPEG loses its location; if that fails it is refused
  File,     // "File": shrunk when it is a picture that decodes, otherwise sent as the file it is
};

/** The most pixels a picture may have to be shrunk (AttachmentLimits::max_image_pixels); the longest edge afterwards. */
inline constexpr int kShrunkImageLongEdge = 2048;

/**
 * Make a picked file ready to send, off the UI thread. The size limit for its kind is checked before the file
 * is read. What a file is comes from its first bytes, not its name. A PNG / JPEG / WebP picture is shrunk (long
 * edge 2048, JPEG 85%, turned upright by its EXIF orientation, no metadata) into `max_image_bytes`; "Original"
 * sends it as it is instead, but a JPEG loses its location and camera data (`StripJpegLocation`). Fail closed:
 * a picture from "Photo" that cannot be decoded, or a JPEG from "Original" that cannot be cleaned, is refused
 * (ImageUnprocessable), never sent. From "File" an undecodable picture goes as the plain file the person chose.
 * A GIF and every other kind go as plain files (a video as it is: .mov / HEVC are not transcoded).
 *
 * `path` may be an Android content:// URI: it is first streamed into `scratch_dir` (removed again before this
 * returns) because decoders and ifstream cannot open it. A pick that carries no name is named from its content.
 */
Roe<OutgoingAttachment> PrepareOutgoingAttachment(const std::string& path, const AttachmentLimits& limits,
                                                  OutgoingPickSource source, const std::string& scratch_dir = {});

} // namespace pbr
