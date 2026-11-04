#pragma once

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What the attachment cache asks of the OS (capability-repos CR014 / CR015): the app installs the platform's
 * (feature/product ProductStack); with none installed, nothing is marked and no poster is made.
 */
struct AttachmentPlatform {
  /** Mark a written file as downloaded from the internet (macOS quarantine, Windows zone). */
  std::function<Roe<void>(const std::string& path)> mark_downloaded;
  /** A JPEG poster from a video file's first frame, no wider or taller than `max_dimension`. */
  std::function<Roe<std::vector<uint8_t>>(const std::string& video_path, int max_dimension)> video_poster_jpeg;
  /** A stand-in poster when the video may not be decoded here (too large for an inline view). */
  std::function<Roe<std::vector<uint8_t>>(int max_dimension)> soft_poster_jpeg;
  /**
   * A JPEG thumbnail of an image's plaintext held in memory (never written out for it; EXIF orientation applied),
   * its long side no more than `max_dimension`; fails for a picture too large to decode safely. With none installed,
   * a large image shows as a file card.
   */
  std::function<Roe<std::vector<uint8_t>>(const std::vector<uint8_t>& image_bytes, int max_dimension)>
      image_thumbnail_jpeg;
};

/** Installs the platform's hooks (once at start; any thread reads them after). */
void SetAttachmentPlatform(AttachmentPlatform platform);
AttachmentPlatform CurrentAttachmentPlatform();

} // namespace pbr
