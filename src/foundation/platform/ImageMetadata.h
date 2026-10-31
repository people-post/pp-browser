#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** What the head of an image file says, read without decoding the picture. */
struct ImageHeaderInfo {
  int width = 0;
  int height = 0;
  /** EXIF orientation 1..8 (JPEG only; 1: upright, also when absent). */
  int exif_orientation = 1;
};

/**
 * Size (and, for JPEG, EXIF orientation) of a PNG, JPEG, GIF or WebP file from its first bytes; nullopt for
 * any other format or a damaged head. Lets a caller refuse a huge picture before it is decoded.
 */
std::optional<ImageHeaderInfo> ReadImageHeader(const std::string& path);
/** The same for bytes already in memory (the head of the file is enough). */
std::optional<ImageHeaderInfo> ReadImageHeaderBytes(const std::vector<uint8_t>& head);

/**
 * A JPEG without its location and camera data: every EXIF / XMP (APP1), IPTC (APP13) and comment segment is
 * removed. A non-upright orientation is kept as a minimal EXIF segment that holds nothing else, so the
 * picture still shows the right way up. Anything that is not a well-formed JPEG is returned unchanged.
 */
std::vector<uint8_t> StripJpegLocation(const std::vector<uint8_t>& jpeg);

} // namespace pbr
